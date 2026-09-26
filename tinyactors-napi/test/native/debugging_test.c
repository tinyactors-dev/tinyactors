/*
 * What a debugger needs from the scheduler (ADR 0027), driven without
 * Node-API: holding an actor between macrosteps and stepping it one message
 * at a time, a hold that waits for a macrostep to end, a withdrawn send()
 * on a held actor, a fault while held, a wake while held, budgets for one
 * actor, operator transitions in the envelope (taken, refused, and for
 * another revision), and settling an abandoned macrostep.
 */
#include <string.h>

#include "check.h"
#include "events.h"
#include "hooks.h"
#include "scheduler.h"
#include "system.h"

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })

static void no_wake(tna_system *system, void *context) {
    (void)system;
    (void)context;
}

enum { MAX_SETTLED = 16 };
static tna_settlement settled[MAX_SETTLED];
static uint32_t settled_count;

static tna_system *make_system(void) {
    settled_count = 0; /* waiter indices start again in each system */
    tna_system *system = NULL;
    CHECK(tna_system_create(&system) == TA_OK);
    system->wake = no_wake;
    return system;
}

static void finish_system(tna_system *system) {
    tna_system_close(system);
    CHECK(tna_system_leaked_bytes(system) == 0);
    tna_system_free(system);
}

static ta_definition_id define(tna_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system->core, &(ta_definition_compile_params){
                                                       .spec = spec,
                                                       .datamodel = ta_null_datamodel(),
                                                       .allocator = tna_allocator_for(&system->allocator, TNA_MEMORY_CORE),
                                                       .out = &definition,
                                                   }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system->core, definition, &id) == TA_OK);
    return id;
}

/* a --x--> b --y--> c --z--> end (a top-level final); state IDs 0 to 3. */
static ta_definition_id define_steps(tna_system *system) {
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

/*
 * idle --go--> s1 -> s2 -> s3 -> rest: three eventless microsteps, each
 * entering a state whose onentry holds two <log> actions. With `spin`,
 * s3 goes back to s1 instead: a macrostep that never ends.
 */
static ta_definition_id define_chain(tna_system *system, bool spin) {
    static ta_scxml_action_spec logs[] = {
        { .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = { "one", 3 } } },
        { .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = { "two", 3 } } },
        { 0 },
    };
    static ta_scxml_handler_spec onentry[] = { { .actions = logs }, { 0 } };
    static ta_str to_s1[] = { { "s1", 2 }, { 0 } };
    static ta_str to_s2[] = { { "s2", 2 }, { 0 } };
    static ta_str to_s3[] = { { "s3", 2 }, { 0 } };
    static ta_str to_rest[] = { { "rest", 4 }, { 0 } };
    static ta_scxml_transition_spec on_go[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { "go", 2 }, .targets = to_s1 }, { 0 } };
    static ta_scxml_transition_spec s1_next[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_s2 }, { 0 } };
    static ta_scxml_transition_spec s2_next[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_s3 }, { 0 } };
    static ta_scxml_transition_spec s3_rest[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_rest }, { 0 } };
    static ta_scxml_transition_spec s3_spin[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_s1 }, { 0 } };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle"), .transitions = on_go },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("s1"), .onentry = onentry, .transitions = s1_next },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("s2"), .onentry = onentry, .transitions = s2_next },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("s3"), .onentry = onentry, .transitions = spin ? s3_spin : s3_rest },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("rest") },
        { 0 },
    };
    ta_scxml_spec spec = { .name = spin ? S("spin") : S("chain"), .states = states };
    return define(system, &spec);
}

static ta_actor_id spawn(tna_system *system, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system->core, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    CHECK(tna_hook_actor_created(system, actor) == TA_OK);
    return actor;
}

static ta_event_name_id name(tna_system *system, const char *text) {
    ta_event_name_id id = 0;
    CHECK(tna_event_name_intern(system->core, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &id) == TA_OK);
    return id;
}

static ta_result post(tna_system *system, ta_actor_id target, ta_event_name_id event) {
    ta_event message = { .name = event, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return tna_system_post(system, &(tna_message){ .target = target, .event = &message });
}

/* Posts `event` with a fresh waiter and returns the waiter. */
static uint32_t send(tna_system *system, ta_actor_id target, ta_event_name_id event) {
    uint32_t waiter = tna_sched_waiter_take(&system->sched);
    CHECK(waiter != 0);
    ta_event message = { .name = event, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(tna_system_post(system, &(tna_message){ .target = target, .event = &message, .waiter = waiter }) == TA_OK);
    return waiter;
}

/* Posts `event` carrying an operator transition; `waiter` may be 0. */
static ta_result post_jump(tna_system *system, ta_actor_id target, ta_event_name_id event, const tna_message_jump *jump,
                           uint32_t waiter) {
    ta_event message = { .name = event, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return tna_system_post(system, &(tna_message){ .target = target, .event = &message, .jump = jump, .waiter = waiter });
}

typedef struct {
    uint32_t held;
    uint32_t faults;
    uint32_t dead;
    uint32_t refused;
} turn_counts;

/* Pops settlements as the Node-API layer does, and gives their waiters back. */
static void collect(tna_system *system) {
    tna_settlement settlement;
    while (tna_sched_settlement_pop(&system->sched, &settlement)) {
        CHECK(settled_count < MAX_SETTLED);
        settled[settled_count] = settlement;
        settled_count += 1u;
        tna_sched_waiter_give(&system->sched, settlement.waiter);
    }
}

/* The latest settlement of `waiter` (waiter indices are reused), or NULL. */
static const tna_settlement *settlement_of(uint32_t waiter) {
    for (uint32_t index = settled_count; index > 0; index -= 1) {
        if (settled[index - 1u].waiter == waiter) {
            return &settled[index - 1u];
        }
    }
    return NULL;
}

/* One turn, counting its records. */
static turn_counts turn(tna_system *system) {
    turn_counts counts = { 0 };
    tna_system_turn(system);
    for (const tna_turn_record *record = system->sched.records; record != NULL; record = record->next) {
        counts.held += record->kind == TNA_RECORD_HELD ? 1u : 0u;
        counts.faults += record->kind == TNA_RECORD_FAULT ? 1u : 0u;
        counts.dead += record->kind == TNA_RECORD_DEAD_LETTER ? 1u : 0u;
        counts.refused += record->kind == TNA_RECORD_DEAD_LETTER && record->reason == TNA_DEAD_REFUSED ? 1u : 0u;
    }
    collect(system);
    tna_system_end_turn(system);
    return counts;
}

/* Runs turns until quiet, adding up their records. */
static turn_counts settle(tna_system *system) {
    turn_counts total = { 0 };
    for (int guard = 0; !tna_system_quiet(system); guard += 1) {
        CHECK(guard < 1000);
        turn_counts counts = turn(system);
        total.held += counts.held;
        total.faults += counts.faults;
        total.dead += counts.dead;
        total.refused += counts.refused;
    }
    return total;
}

static const char *state_of(tna_system *system, ta_actor_id actor) {
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

static tna_sched_state state(tna_system *system, ta_actor_id actor) {
    const tna_sched_entry *entry = tna_sched_lookup(&system->sched, actor);
    return entry == NULL ? TNA_SCHED_NONE : (tna_sched_state)entry->state;
}

static bool hold(tna_system *system, ta_actor_id actor) {
    bool held = false;
    CHECK(tna_sched_hold(&system->sched, &(tna_sched_hold_params){ .actor = actor, .held = &held }) == TA_OK);
    return held;
}

static tna_step_kind step(tna_system *system, ta_actor_id actor) {
    tna_step_kind kind = TNA_STEP_WAITING;
    CHECK(tna_sched_step(&system->sched, &(tna_sched_step_params){ .actor = actor, .out = &kind }) == TA_OK);
    return kind;
}

/* An idle actor is held at once; its mail waits; a step takes exactly one
 * message; letting go delivers the rest. */
static void hold_and_step(void) {
    tna_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    ta_actor_id other = spawn(system, define_steps(system));
    settle(system);
    CHECK(sizeof(tna_sched_entry) == 16u);
    CHECK(sizeof(tna_envelope) == 104u); /* the jump shares the address's room */
    CHECK(hold(system, actor));
    CHECK(state(system, actor) == TNA_SCHED_HELD);
    ta_event_name_id x = name(system, "x");
    ta_event_name_id y = name(system, "y");
    CHECK(post(system, actor, x) == TA_OK);
    CHECK(post(system, actor, y) == TA_OK);
    CHECK(post(system, other, x) == TA_OK);
    turn_counts counts = settle(system);
    CHECK(counts.held == 0u && counts.faults == 0u);
    CHECK(tna_system_quiet(system)); /* a held actor's mail does not keep the system busy */
    CHECK(strcmp(state_of(system, actor), "a") == 0);
    CHECK(strcmp(state_of(system, other), "b") == 0); /* the others run */
    CHECK(tna_sched_mailbox_count(&system->sched, actor) == 2u);

    CHECK(step(system, actor) == TNA_STEP_MESSAGE);
    CHECK(state(system, actor) == TNA_SCHED_READY);
    counts = settle(system);
    CHECK(counts.held == 1u); /* its macrostep ended: it stands still again */
    CHECK(state(system, actor) == TNA_SCHED_HELD);
    CHECK(strcmp(state_of(system, actor), "b") == 0);
    CHECK(tna_sched_mailbox_count(&system->sched, actor) == 1u);

    /* A step of an actor that is not held is refused. */
    CHECK(tna_sched_step(&system->sched, &(tna_sched_step_params){ .actor = other }) == TA_ERR_INVALID);

    CHECK(tna_sched_unhold(&system->sched, actor) == TA_OK);
    CHECK(state(system, actor) == TNA_SCHED_READY);
    counts = settle(system);
    CHECK(counts.held == 0u);
    CHECK(strcmp(state_of(system, actor), "c") == 0);
    CHECK(state(system, actor) == TNA_SCHED_IDLE);

    /* A step with an empty mailbox waits for the next message, and holds after it. */
    CHECK(hold(system, actor));
    CHECK(step(system, actor) == TNA_STEP_WAITING);
    CHECK(state(system, actor) == TNA_SCHED_IDLE);
    CHECK(post(system, actor, name(system, "z")) == TA_OK);
    counts = settle(system);
    CHECK(strcmp(state_of(system, actor), "final") == 0);

    /* A finished actor cannot be held; an unknown one is not found. */
    CHECK(tna_sched_hold(&system->sched, &(tna_sched_hold_params){ .actor = actor }) == TA_ERR_DONE);
    CHECK(tna_sched_hold(&system->sched, &(tna_sched_hold_params){ .actor = ta_actor_id_make(900u, 1u) }) ==
          TA_ERR_NOT_FOUND);
    CHECK(tna_sched_unhold(&system->sched, ta_actor_id_make(900u, 1u)) == TA_ERR_NOT_FOUND);
    finish_system(system);
}

/* A hold on an actor inside a macrostep lets the macrostep end, completes
 * the send() that started it, and holds the actor afterwards. */
static void hold_waits_for_the_macrostep(void) {
    tna_system *system = make_system();
    ta_actor_id actor = spawn(system, define_chain(system, false));
    settle(system);
    system->budgets.microsteps = 1u; /* one microstep per turn */
    uint32_t waiter = send(system, actor, name(system, "go"));
    CHECK(post(system, actor, name(system, "go")) == TA_OK); /* waits behind */
    turn(system); /* delivered */
    turn(system); /* the first microstep */
    CHECK(state(system, actor) == TNA_SCHED_RUNNABLE);
    CHECK(!hold(system, actor)); /* inside a macrostep: held once it ends */
    CHECK(state(system, actor) == TNA_SCHED_RUNNABLE);
    turn_counts counts = settle(system);
    CHECK(counts.held == 1u);
    CHECK(state(system, actor) == TNA_SCHED_HELD);
    CHECK(strcmp(state_of(system, actor), "rest") == 0);
    const tna_settlement *done = settlement_of(waiter);
    CHECK(done != NULL && done->kind == TNA_SETTLE_STABLE); /* the macrostep ended, so the send() completed */
    CHECK(tna_sched_mailbox_count(&system->sched, actor) == 1u);
    finish_system(system);
}

/* The webhook case: a send() whose timeout passes while its actor is held is
 * withdrawn from the mailbox and never delivered; a retry is taken once. */
static void withdrawn_while_held(void) {
    tna_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    CHECK(hold(system, actor));
    ta_event_name_id x = name(system, "x");
    uint32_t waiter = send(system, actor, x);
    settle(system);
    CHECK(tna_sched_cancel(&system->sched, waiter)); /* the timeout */
    tna_sched_waiter_give(&system->sched, waiter);
    CHECK(tna_sched_mailbox_count(&system->sched, actor) == 0u);
    CHECK(system->sched.queued == 0u);
    CHECK(tna_sched_unhold(&system->sched, actor) == TA_OK);
    settle(system);
    CHECK(strcmp(state_of(system, actor), "a") == 0); /* never delivered */
    uint64_t delivered = system->sched.delivered;
    uint32_t retry = send(system, actor, x);
    settle(system);
    CHECK(system->sched.delivered == delivered + 1u);
    CHECK(settlement_of(retry) != NULL && settlement_of(retry)->kind == TNA_SETTLE_STABLE);
    CHECK(strcmp(state_of(system, actor), "b") == 0);
    finish_system(system);
}

/* A fault while held still quarantines and reports; once released, the
 * actor is held again when its macrostep ends. */
static void fault_while_held(void) {
    tna_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    CHECK(hold(system, actor));
    CHECK(post(system, actor, 0x7fffffffu) == TA_OK); /* a name core never issued */
    CHECK(post(system, actor, name(system, "x")) == TA_OK);
    CHECK(step(system, actor) == TNA_STEP_MESSAGE);
    turn_counts counts = settle(system);
    CHECK(counts.faults == 1u && counts.dead == 1u);
    CHECK(state(system, actor) == TNA_SCHED_QUARANTINED);
    CHECK(system->sched.quarantined == 1u);
    CHECK(tna_sched_release(&system->sched, actor) == TA_OK);
    counts = settle(system);
    CHECK(counts.held == 1u);
    CHECK(state(system, actor) == TNA_SCHED_HELD); /* the hold outlived the fault */
    CHECK(system->sched.quarantined == 0u);
    CHECK(tna_sched_mailbox_count(&system->sched, actor) == 1u);
    finish_system(system);
}

/* Core waking a held actor waits for the step, which runs that macrostep
 * instead of a message. */
static void woken_while_held(void) {
    tna_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    CHECK(hold(system, actor));
    CHECK(post(system, actor, name(system, "x")) == TA_OK);
    CHECK(tna_sched_wake(&system->sched, actor) == TA_OK);
    CHECK(state(system, actor) == TNA_SCHED_HELD);
    CHECK(step(system, actor) == TNA_STEP_MACROSTEP);
    CHECK(state(system, actor) == TNA_SCHED_RUNNABLE);
    turn_counts counts = settle(system);
    CHECK(counts.held == 1u);
    CHECK(strcmp(state_of(system, actor), "a") == 0); /* the message still waits */
    CHECK(tna_sched_mailbox_count(&system->sched, actor) == 1u);
    CHECK(tna_sched_unhold(&system->sched, actor) == TA_OK);
    settle(system);
    CHECK(strcmp(state_of(system, actor), "b") == 0);
    finish_system(system);
}

/* Held anywhere, an actor stops where a budget paused it, inside a
 * macrostep; a step runs it to its next pause; letting go finishes it. */
static void held_anywhere(void) {
    tna_system *system = make_system();
    ta_actor_id actor = spawn(system, define_chain(system, false));
    settle(system);
    CHECK(tna_sched_set_budgets(&system->sched, &(tna_sched_budgets_params){
                                                    .actor = actor,
                                                    .budgets = &(tna_actor_budgets){ .microsteps = 64u, .actions = 1u },
                                                }) == TA_OK);
    CHECK(post(system, actor, name(system, "go")) == TA_OK);
    CHECK(post(system, actor, name(system, "go")) == TA_OK); /* ignored in rest: a second message */
    bool held = true;
    CHECK(tna_sched_hold(&system->sched, &(tna_sched_hold_params){ .actor = actor, .anywhere = true, .held = &held }) ==
          TA_OK);
    CHECK(held); /* ready: held at once, before its first message */
    CHECK(step(system, actor) == TNA_STEP_MESSAGE);
    turn_counts counts = turn(system); /* delivered: it stands still before the macrostep begins */
    CHECK(counts.held == 1u);
    CHECK(state(system, actor) == TNA_SCHED_HELD);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK && view.macrostep_in_progress && view.step.macrostep == 0);
    CHECK(step(system, actor) == TNA_STEP_MACROSTEP);
    counts = turn(system); /* the first unit, then a pause */
    CHECK(counts.held == 1u);
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK && view.macrostep_in_progress && view.step.macrostep > 0);
    counts = settle(system);
    CHECK(counts.held == 0u); /* it stands still inside the macrostep */
    uint32_t units = 1;
    for (;;) {
        CHECK(units < 20u);
        tna_step_kind kind = step(system, actor);
        if (kind != TNA_STEP_MACROSTEP) {
            CHECK(kind == TNA_STEP_MESSAGE); /* the macrostep ended: the next step takes the next message */
            break;
        }
        counts = settle(system);
        CHECK(counts.held == 1u);
        units += 1u;
    }
    CHECK(units >= 4u);
    CHECK(strcmp(state_of(system, actor), "rest") == 0);
    CHECK(tna_sched_unhold(&system->sched, actor) == TA_OK);
    settle(system);
    CHECK(state(system, actor) == TNA_SCHED_IDLE);
    CHECK((tna_sched_lookup(&system->sched, actor)->flags & (TNA_SCHED_HOLD | TNA_SCHED_ANYWHERE)) == 0);

    /* A runnable actor held anywhere stands still at once. */
    ta_actor_id fresh = spawn(system, define_steps(system));
    CHECK(state(system, fresh) == TNA_SCHED_RUNNABLE); /* its bootstrap waits */
    CHECK(tna_sched_hold(&system->sched, &(tna_sched_hold_params){ .actor = fresh, .anywhere = true, .held = &held }) ==
          TA_OK);
    CHECK(held && state(system, fresh) == TNA_SCHED_HELD);
    settle(system);
    CHECK(strcmp(state_of(system, fresh), "") == 0); /* not bootstrapped yet */
    CHECK(step(system, fresh) == TNA_STEP_MACROSTEP);
    settle(system);
    CHECK(strcmp(state_of(system, fresh), "a") == 0);

    /* Abandoned while held inside a macrostep, it has nothing left to run. */
    ta_actor_id spinner = spawn(system, define_chain(system, true));
    settle(system);
    CHECK(tna_sched_hold(&system->sched, &(tna_sched_hold_params){ .actor = spinner, .anywhere = true }) == TA_OK);
    CHECK(post(system, spinner, name(system, "go")) == TA_OK);
    CHECK(step(system, spinner) == TNA_STEP_MESSAGE);
    turn(system); /* delivered */
    CHECK(step(system, spinner) == TNA_STEP_MACROSTEP);
    for (int index = 0; index < 4 && state(system, spinner) != TNA_SCHED_HELD; index += 1) {
        turn(system); /* the system's budgets pause between microsteps */
    }
    CHECK(state(system, spinner) == TNA_SCHED_HELD);
    CHECK((tna_sched_lookup(&system->sched, spinner)->flags & TNA_SCHED_UNFINISHED) != 0);
    CHECK(ta_actor_abandon(system->core, spinner) == TA_OK);
    CHECK(tna_hook_actor_abandoned(system, spinner) == TA_OK);
    CHECK((tna_sched_lookup(&system->sched, spinner)->flags & TNA_SCHED_UNFINISHED) == 0);
    CHECK(step(system, spinner) == TNA_STEP_WAITING);
    finish_system(system);
}

/* Turns until the actor stands between macrosteps again. */
static uint32_t turns_to_stable(tna_system *system, ta_actor_id actor) {
    uint32_t turns = 0;
    while (state(system, actor) == TNA_SCHED_RUNNABLE || state(system, actor) == TNA_SCHED_READY) {
        CHECK(turns < 100u);
        turn(system);
        turns += 1u;
    }
    return turns;
}

/* One actor resumes with budgets of its own; the others keep the system's. */
static void budgets_for_one_actor(void) {
    tna_system *system = make_system();
    ta_definition_id chain = define_chain(system, false);
    ta_actor_id debugged = spawn(system, chain);
    ta_actor_id other = spawn(system, chain);
    settle(system);
    CHECK(tna_sched_set_budgets(&system->sched, &(tna_sched_budgets_params){
                                                    .actor = debugged,
                                                    .budgets = &(tna_actor_budgets){ .microsteps = 64u, .actions = 1u },
                                                }) == TA_OK);
    CHECK((tna_sched_lookup(&system->sched, debugged)->flags & TNA_SCHED_OWN_BUDGETS) != 0);
    ta_event_name_id go = name(system, "go");
    CHECK(post(system, debugged, go) == TA_OK);
    CHECK(post(system, other, go) == TA_OK);
    /* The other actor: delivered, then one resume runs its whole macrostep. */
    turn(system);
    turn(system);
    CHECK(state(system, other) == TNA_SCHED_IDLE);
    CHECK(strcmp(state_of(system, other), "rest") == 0);
    /* The debugged one runs one unit per turn: six <log> actions, and the
     * first unit of each microstep runs before any pause (core ADR 0061). */
    CHECK(state(system, debugged) == TNA_SCHED_RUNNABLE);
    uint32_t turns = turns_to_stable(system, debugged);
    CHECK(turns >= 3u);
    CHECK(strcmp(state_of(system, debugged), "rest") == 0);

    /* Cleared, it runs with the system's budgets again. */
    CHECK(tna_sched_set_budgets(&system->sched, &(tna_sched_budgets_params){ .actor = debugged }) == TA_OK);
    CHECK(tna_map_count(&system->sched.budgets) == 0u);
    CHECK(tna_sched_set_budgets(&system->sched, &(tna_sched_budgets_params){
                                                    .actor = debugged,
                                                    .budgets = &(tna_actor_budgets){ .microsteps = 0u },
                                                }) == TA_ERR_INVALID);
    CHECK(tna_sched_set_budgets(&system->sched, &(tna_sched_budgets_params){
                                                    .actor = ta_actor_id_make(900u, 1u),
                                                    .budgets = &(tna_actor_budgets){ .microsteps = 1u },
                                                }) == TA_ERR_NOT_FOUND);

    /* Destroying the actor forgets its budgets. */
    CHECK(tna_sched_set_budgets(&system->sched, &(tna_sched_budgets_params){
                                                    .actor = debugged,
                                                    .budgets = &(tna_actor_budgets){ .microsteps = 1u },
                                                }) == TA_OK);
    CHECK(tna_map_count(&system->sched.budgets) == 1u);
    CHECK(ta_actor_destroy(system->core, debugged) == TA_OK);
    CHECK(tna_hook_actor_removed(system, debugged) == TA_OK);
    CHECK(tna_map_count(&system->sched.budgets) == 0u);
    finish_system(system);
}

static ta_definition_id definition_of(tna_system *system, ta_actor_id actor) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    return view.definition_id;
}

/* An operator transition queues behind real mail and reaches deliver; one
 * the actor no longer accepts is a dead letter, not a fault. */
static void operator_transitions(void) {
    tna_system *system = make_system();
    ta_definition_id steps = define_steps(system);
    ta_actor_id actor = spawn(system, steps);
    settle(system);
    ta_event_name_id x = name(system, "x");
    ta_event_name_id jump_event = name(system, "operator.goto");
    ta_state_id to_c[] = { 2u };
    tna_message_jump to_c_from_root = {
        .definition = steps,
        .transition = { .source = TA_STATE_NONE, .targets = to_c, .target_count = 1u },
    };
    /* Behind x: x moves a to b first, then the jump moves b to c. */
    CHECK(post(system, actor, x) == TA_OK);
    uint32_t waiter = tna_sched_waiter_take(&system->sched);
    CHECK(post_jump(system, actor, jump_event, &to_c_from_root, waiter) == TA_OK);
    const tna_envelope *queued = tna_sched_envelope(&system->sched, tna_sched_lookup(&system->sched, actor)->last);
    CHECK(queued->has_jump && queued->jump->count == 1u && queued->jump->targets[0] == 2u);
    turn_counts counts = settle(system);
    CHECK(counts.faults == 0u && counts.dead == 0u);
    CHECK(strcmp(state_of(system, actor), "c") == 0);
    CHECK(settlement_of(waiter) != NULL && settlement_of(waiter)->kind == TNA_SETTLE_STABLE);

    /* From a: queued behind a jump to c, a is no longer active: refused. */
    ta_state_id to_b[] = { 1u };
    tna_message_jump from_a = {
        .definition = steps,
        .transition = { .source = 0u, .targets = to_b, .target_count = 1u },
    };
    CHECK(post(system, actor, name(system, "unknown.event")) == TA_OK); /* ignored: the actor stays in c */
    waiter = tna_sched_waiter_take(&system->sched);
    CHECK(post_jump(system, actor, jump_event, &from_a, waiter) == TA_OK);
    counts = settle(system);
    CHECK(counts.faults == 0u && counts.refused == 1u);
    CHECK(state(system, actor) == TNA_SCHED_IDLE); /* not quarantined */
    CHECK(strcmp(state_of(system, actor), "c") == 0);
    const tna_settlement *refused = settlement_of(waiter);
    CHECK(refused != NULL && refused->kind == TNA_SETTLE_DEAD && refused->reason == TNA_DEAD_REFUSED);

    /* A jump over another revision's state IDs is refused without reaching core. */
    ta_definition_id other = define_chain(system, false);
    tna_message_jump foreign = {
        .definition = other,
        .transition = { .source = TA_STATE_NONE, .targets = to_b, .target_count = 1u },
    };
    CHECK(post_jump(system, actor, jump_event, &foreign, 0) == TA_OK);
    counts = settle(system);
    CHECK(counts.faults == 0u && counts.refused == 1u);
    CHECK(strcmp(state_of(system, actor), "c") == 0);

    /* A jump that a held actor takes on a step holds it again afterwards. */
    CHECK(definition_of(system, actor) == steps);
    CHECK(hold(system, actor));
    ta_state_id to_a[] = { 0u };
    tna_message_jump back = {
        .definition = steps,
        .transition = { .source = TA_STATE_NONE, .targets = to_a, .target_count = 1u },
    };
    CHECK(post_jump(system, actor, jump_event, &back, 0) == TA_OK);
    CHECK(step(system, actor) == TNA_STEP_MESSAGE);
    counts = settle(system);
    CHECK(counts.held == 1u);
    CHECK(strcmp(state_of(system, actor), "a") == 0);

    /* Jumps left waiting, withdrawn, or to a gone actor give their memory back. */
    uint32_t withdrawn = tna_sched_waiter_take(&system->sched);
    CHECK(post_jump(system, actor, jump_event, &back, withdrawn) == TA_OK);
    CHECK(tna_sched_cancel(&system->sched, withdrawn));
    tna_sched_waiter_give(&system->sched, withdrawn);
    CHECK(post_jump(system, actor, jump_event, &back, 0) == TA_OK); /* left in the held mailbox */
    CHECK(post_jump(system, ta_actor_id_make(900u, 1u), jump_event, &back, 0) == TA_OK);
    counts = settle(system);
    CHECK(counts.dead == 1u);
    finish_system(system); /* no leak: the waiting jump went with its envelope */
}

/* Abandoning a macrostep that never ends settles napi's state: the send()
 * rejects as abandoned, and the actor stands between macrosteps. */
static void abandoned_macrostep(void) {
    tna_system *system = make_system();
    ta_definition_id spin = define_chain(system, true);
    ta_actor_id actor = spawn(system, spin);
    ta_actor_id held = spawn(system, spin);
    settle(system);
    ta_event_name_id go = name(system, "go");
    uint32_t waiter = send(system, actor, go);
    CHECK(post(system, actor, go) == TA_OK); /* waits behind the runaway */
    CHECK(post(system, held, go) == TA_OK);
    for (int index = 0; index < 5; index += 1) {
        turn(system);
    }
    CHECK(state(system, actor) == TNA_SCHED_RUNNABLE);
    CHECK(settlement_of(waiter) == NULL);

    /* Paused inside a microstep, core refuses; budgets of its own let it
     * finish that microstep and pause before the next (core ADR 0082). */
    system->budgets.actions = 1u;
    turn(system);
    CHECK(ta_actor_abandon(system->core, actor) == TA_ERR_BUSY);
    CHECK(tna_sched_set_budgets(&system->sched, &(tna_sched_budgets_params){
                                                    .actor = actor,
                                                    .budgets = &(tna_actor_budgets){ .microsteps = 1u, .actions = 0u },
                                                }) == TA_OK);
    turn(system);
    CHECK(ta_actor_abandon(system->core, actor) == TA_OK);
    CHECK(tna_hook_actor_abandoned(system, actor) == TA_OK);
    CHECK(tna_sched_set_budgets(&system->sched, &(tna_sched_budgets_params){ .actor = actor }) == TA_OK);
    collect(system);
    const tna_settlement *abandoned = settlement_of(waiter);
    CHECK(abandoned != NULL && abandoned->kind == TNA_SETTLE_ABANDONED);
    CHECK(state(system, actor) == TNA_SCHED_READY); /* its next message waits */
    turn(system); /* the stale runnable entry is skipped; the next go starts another runaway */
    CHECK(state(system, actor) == TNA_SCHED_RUNNABLE);
    CHECK(tna_sched_mailbox_count(&system->sched, actor) == 0u);

    /* A hold waiting for a runaway to end takes effect when it is abandoned. */
    system->budgets.actions = 0u;
    turn(system);
    CHECK(state(system, held) == TNA_SCHED_RUNNABLE);
    CHECK(!hold(system, held));
    for (int index = 0; index < 3; index += 1) {
        turn(system);
    }
    CHECK(state(system, held) == TNA_SCHED_RUNNABLE);
    CHECK(ta_actor_abandon(system->core, held) == TA_OK);
    CHECK(tna_hook_actor_abandoned(system, held) == TA_OK);
    CHECK(state(system, held) == TNA_SCHED_HELD);
    turn(system);
    CHECK(state(system, held) == TNA_SCHED_HELD);
    finish_system(system);
}

int main(void) {
    hold_and_step();
    hold_waits_for_the_macrostep();
    withdrawn_while_held();
    fault_while_held();
    woken_while_held();
    held_anywhere();
    budgets_for_one_actor();
    operator_transitions();
    abandoned_macrostep();
    puts("debugging_test passed");
    return 0;
}
