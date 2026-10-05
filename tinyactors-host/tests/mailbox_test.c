/*
 * Mailboxes over core's pool, held mail and dead letters (ADRs 0005, 0008,
 * 0014; core ADR 0099), driven without Node-API: per-target FIFO across the
 * turn's budget, mail before bootstrap, stale IDs, quarantine with an empty
 * mailbox, finished and destroyed targets, backpressure from core's full
 * pool, a refused message left to its caller, mail between actors held past
 * the limits (kernel ADR 0003) and past a full pool, a released actor's
 * mail past overflow waiting for room, the dead-letter budget, a fault that
 * stays with its actor, and no allocator call in steady state.
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

static tah_system *make_system_with(const ta_param_value *params, uint32_t count) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform(), .params = params,
                                                          .param_count = count },
                            &system) == TA_OK);
    tah_system_set_driver(system, &(tah_pump_driver){ .wake = no_wake });
    return system;
}

static tah_system *make_system(void) {
    return make_system_with(NULL, 0);
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

/* One turn; returns the number of records of `kind` it produced. */
static uint32_t turn(tah_system *system, tah_record_kind kind) {
    tah_system_turn(system);
    uint32_t count = 0;
    for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
        count += record->kind == kind ? 1u : 0u;
    }
    tah_system_end_turn(system);
    return count;
}

/* Runs turns until quiet; returns the dead letters and checks for no faults. */
static uint32_t settle(tah_system *system) {
    uint32_t dead = 0;
    for (int guard = 0; !tah_system_quiet(system); guard += 1) {
        CHECK(guard < 1000);
        tah_system_turn(system);
        for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
            CHECK(record->kind != TAH_RECORD_FAULT);
            dead += record->kind == TAH_RECORD_DEAD_LETTER ? 1u : 0u;
        }
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

static tah_sched_state state(tah_system *system, ta_actor_id actor) {
    const tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    return entry == NULL ? TAH_SCHED_NONE : (tah_sched_state)entry->state;
}

/* Per-target FIFO holds when a turn stops at its message budget. */
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
        CHECK(state(system, actors[index]) == TAH_SCHED_READY);
        CHECK(tah_sched_mailbox_count(&system->sched, actors[index]) == 2u);
    }
    /* Two ta_run() calls, each taking one message. */
    system->budgets.resumes = 2u;
    tah_system_turn(system);
    CHECK(system->sched.delivered == 2u); /* the budget stopped the turn */
    tah_system_end_turn(system);
    system->budgets.resumes = 1024u;
    CHECK(settle(system) == 0u);
    for (int index = 0; index < COUNT; index += 1) {
        CHECK(strcmp(state_of(system, actors[index]), "c") == 0);
        CHECK(state(system, actors[index]) == TAH_SCHED_IDLE);
    }
    CHECK(system->sched.delivered == 2u * COUNT);
    CHECK(system->sched.queued == 0u);
    finish_system(system);
}

/* Mail posted between spawn and bootstrap waits for the bootstrap. */
static void mail_before_bootstrap(void) {
    tah_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    CHECK(state(system, actor) == TAH_SCHED_RUNNABLE);
    CHECK(post(system, actor, name(system, "x")) == TA_OK);
    CHECK(state(system, actor) == TAH_SCHED_RUNNABLE); /* only collects mail */
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, actor), "b") == 0);
    finish_system(system);
}

/* A stale ID's mail is a dead letter at once and never reaches the slot's next actor. */
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
    CHECK(tah_sched_mailbox_count(&system->sched, next) == 0u);
    CHECK(system->sched.dead_count == 1u);
    tah_system_turn(system);
    const tah_turn_record *record = system->sched.records;
    CHECK(record != NULL && record->kind == TAH_RECORD_DEAD_LETTER);
    CHECK(record->actor == old && record->reason == TAH_DEAD_NOT_FOUND && record->event == name(system, "x"));
    tah_system_end_turn(system);
    CHECK(strcmp(state_of(system, next), "a") == 0);
    CHECK(system->sched.dead_letters == 1u);
    finish_system(system);
}

/* A quarantined actor with an empty mailbox stays out when mail arrives; release brings it back. */
static void quarantine(void) {
    tah_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    CHECK(post(system, actor, 0x7fffffffu) == TA_OK); /* core refuses it: the actor is quarantined */
    tah_system_turn(system);
    tah_system_end_turn(system);
    CHECK(state(system, actor) == TAH_SCHED_QUARANTINED);
    settle(system);
    CHECK(post(system, actor, name(system, "x")) == TA_OK);
    CHECK(state(system, actor) == TAH_SCHED_QUARANTINED);
    CHECK(tah_system_quiet(system)); /* nothing to do: the mail waits */
    CHECK(strcmp(state_of(system, actor), "a") == 0);
    CHECK(tah_sched_release(&system->sched, actor) == TA_OK);
    tah_system_want_turn(system);
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, actor), "b") == 0);
    CHECK(state(system, actor) == TAH_SCHED_IDLE);
    finish_system(system);
}

/* A delivery core refuses as malformed faults that actor only. */
static void a_fault_stays_with_its_actor(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    ta_actor_id bad = spawn(system, chart);
    ta_actor_id good = spawn(system, chart);
    settle(system);
    CHECK(post(system, bad, 0x7fffffffu) == TA_OK); /* a name core never issued */
    CHECK(post(system, good, name(system, "x")) == TA_OK);
    tah_system_turn(system);
    const tah_turn_record *record = system->sched.records;
    CHECK(record != NULL && record->kind == TAH_RECORD_FAULT && record->actor == bad);
    CHECK(strcmp(record->operation, "deliver") == 0 && record->code == TA_ERR_INVALID);
    uint32_t refused = 0;
    for (; record != NULL; record = record->next) {
        refused += record->kind == TAH_RECORD_DEAD_LETTER && record->reason == TAH_DEAD_REFUSED ? 1u : 0u;
    }
    CHECK(refused == 1u); /* the refused envelope, not the actor's other mail */
    tah_system_end_turn(system);
    CHECK(settle(system) == 0u);
    CHECK(state(system, bad) == TAH_SCHED_QUARANTINED);
    CHECK(strcmp(state_of(system, good), "b") == 0);
    /* Mail for the quarantined actor is kept, not delivered. */
    CHECK(post(system, bad, name(system, "x")) == TA_OK);
    CHECK(settle(system) == 0u);
    CHECK(tah_sched_mailbox_count(&system->sched, bad) == 1u);
    /* Destroying it discards its mailbox as dead letters. */
    destroy(system, bad);
    CHECK(settle(system) == 1u);
    finish_system(system);
}

/* Mail for a finished actor, and for a destroyed one, becomes dead letters. */
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
    /* Mail posted to the retained finished actor is dead at once. */
    CHECK(post(system, actor, x) == TA_OK);
    CHECK(system->sched.dead_count == 1u);
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

/* Core's pool at its watermark refuses new mail; accepted mail is never dropped. */
static void backpressure(void) {
    /* 8 envelopes, 2 of them for admission. */
    ta_param_value small[] = { { TA_PARAM_POOL_ENVELOPES, 8 }, { TA_PARAM_ADMISSION_WATERMARK, 25 } };
    tah_system *system = make_system_with(small, 2);
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    ta_event_name_id x = name(system, "x");
    CHECK(post(system, actor, x) == TA_OK);
    CHECK(post(system, actor, name(system, "y")) == TA_OK);
    CHECK(post(system, actor, x) == TA_ERR_FULL);
    CHECK(tah_sched_mailbox_count(&system->sched, actor) == 2u);
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, actor), "c") == 0);
    CHECK(post(system, actor, x) == TA_OK); /* room again */
    settle(system);
    finish_system(system);
}

/* A value whose references the test counts, as a binding's value box. */
typedef struct {
    int refs;
} counted_value;

static void counted_retain(void *self) {
    ((counted_value *)self)->refs += 1;
}

static void counted_release(void *self) {
    ((counted_value *)self)->refs -= 1;
}

static const ta_value_vtable counted_vtable = { .retain = counted_retain, .release = counted_release };

/*
 * A message core's full pool refuses stays the caller's: its data keeps the
 * caller's reference, untouched, its send() waiter is the caller's to give
 * back, and the scheduler's counters and envelopes are as before.
 */
static void refused_post_leaves_the_event_to_its_caller(void) {
    ta_param_value small[] = { { TA_PARAM_POOL_ENVELOPES, 8 }, { TA_PARAM_ADMISSION_WATERMARK, 25 } };
    tah_system *system = make_system_with(small, 2);
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    ta_event_name_id x = name(system, "x");
    CHECK(post(system, actor, x) == TA_OK);
    CHECK(post(system, actor, x) == TA_OK);
    counted_value box = { .refs = 1 };
    ta_event message = { .name = x, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE,
                         .data = { .self = &box, .v = &counted_vtable } };
    ta_trace_parent trace = { .context = { .trace_id_high = 1u, .trace_id_low = 2u, .flags = TA_TRACE_FLAG_SAMPLED } };
    uint32_t waiter = tah_sched_waiter_take(&system->sched);
    CHECK(waiter != 0);
    uint32_t queued = system->sched.queued;
    uint64_t retained = system->sched.retained;
    uint32_t live = system->sched.envelopes.live;
    CHECK(tah_system_post(system, &(tah_message){ .target = actor, .event = &message, .bytes = 40, .waiter = waiter,
                                                  .trace = &trace }) == TA_ERR_FULL);
    CHECK(box.refs == 1);
    CHECK(system->sched.queued == queued && system->sched.retained == retained);
    CHECK(system->sched.envelopes.live == live);
    CHECK(tah_sched_mailbox_count(&system->sched, actor) == 2u);
    tah_sched_waiter_give(&system->sched, waiter); /* the caller's, as after any refusal */
    ta_value_release(&message.data);
    CHECK(box.refs == 0);
    CHECK(settle(system) == 0u);
    finish_system(system);
}

/* Mail between actors, as the router posts it: past the limits it overflows (kernel ADR 0003). */
static ta_result post_between(tah_system *system, ta_actor_id target, ta_event_name_id event) {
    ta_event message = { .name = event, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return tah_system_post(system, &(tah_message){ .target = target, .event = &message, .may_overflow = true });
}

static bool counted(tah_system *system, uint32_t queued, uint32_t overflow) {
    tah_sched_stats stats;
    tah_sched_read_stats(&system->sched, &stats);
    return stats.queued == queued && stats.overflow == overflow;
}

static bool head_overflows(tah_system *system, ta_actor_id actor) {
    return tah_sched_queued_message(&system->sched, tah_sched_mailbox_first(&system->sched, actor)).overflow;
}

/*
 * Past max_queued, mail between actors is accepted as overflow, held in
 * arrival order, while the application's mail is admitted, the limits
 * notwithstanding. Nothing is admitted from the overflow while there is no
 * room, and a system with nothing else to do is quiet. Room goes to the
 * actors in the order they asked for it; overflow to an actor that goes is
 * a dead letter like the rest of its mailbox.
 */
static void overflow_between_actors(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    ta_actor_id hog = spawn(system, chart);
    ta_actor_id first = spawn(system, chart);
    ta_actor_id second = spawn(system, chart);
    ta_actor_id gone = spawn(system, chart);
    settle(system);
    CHECK(post(system, hog, 0x7fffffffu) == TA_OK); /* core refuses it: the hog is quarantined */
    CHECK(turn(system, TAH_RECORD_FAULT) == 1u);
    CHECK(state(system, hog) == TAH_SCHED_QUARANTINED);
    CHECK(settle(system) == 0u); /* its refused message was reported with the fault */
    system->sched.max_queued = 1u;
    ta_event_name_id x = name(system, "x");
    ta_event_name_id y = name(system, "y");
    CHECK(post(system, hog, x) == TA_OK); /* the queue is full, and the hog collects it */
    CHECK(post_between(system, first, x) == TA_OK);
    CHECK(post_between(system, second, x) == TA_OK);
    CHECK(post_between(system, first, y) == TA_OK);
    CHECK(post_between(system, gone, x) == TA_OK);
    CHECK(counted(system, 5u, 4u));
    CHECK(tah_sched_mailbox_count(&system->sched, first) == 2u);
    CHECK(head_overflows(system, first) && tah_sched_queued_message(&system->sched, tah_sched_mailbox_first(&system->sched, first)).held);
    CHECK(settle(system) == 0u); /* they wait for room, and the system is quiet */
    CHECK(strcmp(state_of(system, first), "a") == 0 && strcmp(state_of(system, second), "a") == 0);

    destroy(system, gone); /* its overflow is a dead letter, and makes no room */
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 1u);
    CHECK(counted(system, 4u, 3u));
    destroy(system, hog); /* its mail goes as a dead letter, and makes room for one */
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 1u);
    CHECK(counted(system, 3u, 3u));
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 0u); /* first asked first, and takes it */
    CHECK(strcmp(state_of(system, first), "b") == 0 && strcmp(state_of(system, second), "a") == 0);
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, first), "c") == 0 && strcmp(state_of(system, second), "b") == 0);
    CHECK(counted(system, 0u, 0u));
    /* The application's mail is admitted past the limits. */
    CHECK(post_between(system, second, x) == TA_OK);
    CHECK(post(system, first, name(system, "z")) == TA_OK);
    settle(system);
    CHECK(strcmp(state_of(system, first), "final") == 0);
    finish_system(system);
}

/*
 * With core's pool at its watermark, mail between actors is held, in
 * order, behind the target's earlier held mail, and admitted once a run
 * makes room; the application's mail is refused instead.
 */
static void held_while_the_pool_is_full(void) {
    ta_param_value small[] = { { TA_PARAM_POOL_ENVELOPES, 8 }, { TA_PARAM_ADMISSION_WATERMARK, 25 } };
    tah_system *system = make_system_with(small, 2);
    ta_definition_id chart = define_steps(system);
    ta_actor_id actor = spawn(system, chart);
    settle(system);
    ta_event_name_id x = name(system, "x");
    ta_event_name_id y = name(system, "y");
    ta_event_name_id z = name(system, "z");
    CHECK(post(system, actor, name(system, "noise")) == TA_OK);
    CHECK(post_between(system, actor, x) == TA_OK);
    CHECK(post_between(system, actor, y) == TA_OK); /* held */
    CHECK(post(system, actor, z) == TA_ERR_FULL);
    CHECK(post_between(system, actor, z) == TA_OK); /* held, behind y */
    CHECK(tah_sched_mailbox_count(&system->sched, actor) == 4u);
    CHECK(!tah_sched_queued_message(&system->sched, tah_sched_mailbox_first(&system->sched, actor)).held);
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, actor), "final") == 0); /* x, y, then z */
    CHECK(counted(system, 0u, 0u));
    finish_system(system);
}

/*
 * Mail a quarantined actor collected needs no room under the limits: once
 * the actor is released it is admitted, though overflow ahead of it waits
 * for room, and taking it makes the room the overflow waits for.
 */
static void released_mail_passes_waiting_overflow(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    ta_actor_id holder = spawn(system, chart);
    ta_actor_id other = spawn(system, chart);
    settle(system);
    CHECK(post(system, holder, 0x7fffffffu) == TA_OK); /* core refuses it: the holder is quarantined */
    CHECK(turn(system, TAH_RECORD_FAULT) == 1u);
    CHECK(settle(system) == 0u);
    system->sched.max_retained = 100u;
    ta_event_name_id x = name(system, "x");
    ta_event message = { .name = x, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(tah_system_post(system, &(tah_message){ .target = holder, .event = &message, .bytes = 120 }) == TA_OK);
    CHECK(post_between(system, other, x) == TA_OK); /* overflow: 120 retained already */
    CHECK(settle(system) == 0u); /* it waits, and the system is quiet */
    CHECK(counted(system, 2u, 1u));
    CHECK(tah_sched_release(&system->sched, holder) == TA_OK);
    CHECK(!tah_system_quiet(system));
    CHECK(settle(system) == 0u);
    CHECK(counted(system, 0u, 0u));
    CHECK(strcmp(state_of(system, holder), "b") == 0 && strcmp(state_of(system, other), "b") == 0);
    finish_system(system);
}

/* At most `messages` dead letters are reported per turn. */
static void dead_letter_budget(void) {
    tah_system *system = make_system();
    ta_actor_id gone = spawn(system, define_steps(system));
    settle(system);
    destroy(system, gone);
    ta_event_name_id x = name(system, "x");
    for (int index = 0; index < 10; index += 1) {
        CHECK(post(system, gone, x) == TA_OK);
    }
    system->budgets.messages = 3u;
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 3u);
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 3u);
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 3u);
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 1u);
    CHECK(tah_system_quiet(system));
    CHECK(system->sched.dead_letters == 10u);
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

/* After warm-up, posting and delivering makes no allocator call in the addon (ADR 0014). */
static void steady_state(void) {
    tah_system *system = make_system();
    /* A chart that accepts `ping` forever. */
    ta_str to_idle[] = { S("idle"), { 0 } };
    ta_scxml_transition_spec on_ping[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("ping"), .targets = to_idle }, { 0 } };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle"), .transitions = on_ping }, { 0 } };
    ta_scxml_spec spec = { .name = S("ping"), .states = states };
    ta_definition_id chart = define(system, &spec);
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

/*
 * Replacement in place (ROADMAP step 14): the incumbent's waiting mail moves,
 * in order, to the front of the replacement's mailbox, ahead of mail posted
 * to the replacement directly.
 */
static void transfer_on_replacement(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    ta_actor_id incumbent = spawn(system, chart);
    ta_actor_id replacement = spawn(system, chart);
    settle(system);
    CHECK(post(system, incumbent, name(system, "x")) == TA_OK);
    CHECK(post(system, incumbent, name(system, "y")) == TA_OK);
    CHECK(post(system, replacement, name(system, "z")) == TA_OK);
    CHECK(state(system, incumbent) == TAH_SCHED_READY);
    CHECK(tah_sched_transfer(&system->sched, &(tah_sched_transfer_params){ .from = incumbent, .to = replacement }) ==
          TA_OK);
    CHECK(tah_sched_mailbox_count(&system->sched, incumbent) == 0u);
    CHECK(tah_sched_mailbox_count(&system->sched, replacement) == 3u);
    CHECK(state(system, incumbent) == TAH_SCHED_IDLE);
    CHECK(tah_sched_transfer(&system->sched, &(tah_sched_transfer_params){ .from = incumbent, .to = replacement }) ==
          TA_OK); /* nothing left to move */
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, incumbent), "a") == 0);
    CHECK(strcmp(state_of(system, replacement), "final") == 0); /* x, y, then z */
    CHECK(tah_sched_transfer(&system->sched, &(tah_sched_transfer_params){ .from = 99u, .to = replacement }) ==
          TA_ERR_NOT_FOUND);
    finish_system(system);
}

/* Unloading an actor with mail waiting dead-letters that mail as `unloaded`. */
static void unload_with_mail(void) {
    tah_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    CHECK(post(system, actor, name(system, "x")) == TA_OK);
    CHECK(tah_hook_actor_unloaded(system, actor) == TA_OK); /* as after ta_actor_unload */
    tah_system_turn(system);
    uint32_t unloaded = 0;
    for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
        unloaded += record->kind == TAH_RECORD_DEAD_LETTER && record->reason == TAH_DEAD_UNLOADED ? 1u : 0u;
    }
    tah_system_end_turn(system);
    CHECK(unloaded == 1u);
    CHECK(tah_sched_lookup(&system->sched, actor) == NULL);
    destroy(system, actor); /* core still has it: the hook alone was driven */
    finish_system(system);
}

/* Close releases queued mail and leaks nothing. */
static void close_with_mail(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    ta_actor_id actor = spawn(system, chart);
    ta_actor_id gone = spawn(system, chart);
    settle(system);
    destroy(system, gone);
    for (int index = 0; index < 3000; index += 1) {
        CHECK(post(system, actor, name(system, "y")) == TA_OK); /* spans several envelope pages */
    }
    CHECK(post(system, gone, name(system, "x")) == TA_OK);
    finish_system(system);
}

int main(void) {
    fifo_across_the_budget();
    mail_before_bootstrap();
    stale_ids();
    quarantine();
    a_fault_stays_with_its_actor();
    finished_and_destroyed_targets();
    backpressure();
    refused_post_leaves_the_event_to_its_caller();
    overflow_between_actors();
    held_while_the_pool_is_full();
    released_mail_passes_waiting_overflow();
    dead_letter_budget();
    steady_state();
    close_with_mail();
    transfer_on_replacement();
    unload_with_mail();
    puts("mailbox_test passed");
    return 0;
}
