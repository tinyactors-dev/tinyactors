/*
 * The scheduling table and the runnable part of a turn (ADRs 0005, 0007,
 * 0008), driven without Node-API: entries in fixed pages, stale queue
 * entries skipped, coalesced wake-ups, bootstrap, budget pauses and done
 * records, and no leak at close.
 */
#include "check.h"
#include <string.h>

#include "tinyactors_host/hooks.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "scheduler_private.h"
#include "system_private.h"

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })

static void table_pages(void) {
    tah_allocator allocator;
    tah_allocator_init(&allocator, check_platform_allocator());
    tah_sched sched;
    tah_sched_init(&sched, tah_allocator_for(&allocator, TAH_MEMORY_SCHEDULING),
                   tah_allocator_for(&allocator, TAH_MEMORY_ENVELOPES));
    CHECK(sizeof(tah_sched_entry) == 16u);
    CHECK(tah_sched_entry_at(&sched, 0) == NULL);

    /* A slot in the second page allocates that page only. */
    ta_actor_id far = ta_actor_id_make(TAH_SCHED_PAGE_ENTRIES + 5u, 3u);
    CHECK(tah_sched_add(&sched, far) == TA_OK);
    CHECK(tah_sched_entry_at(&sched, 0) == NULL);
    tah_sched_entry *entry = tah_sched_lookup(&sched, far);
    CHECK(entry != NULL && entry->state == TAH_SCHED_RUNNABLE && entry->generation == 3u);
    CHECK(sched.known == 1u);
    CHECK(!tah_sched_quiet(&sched));

    /* Another generation in the same slot is not this actor. */
    CHECK(tah_sched_lookup(&sched, ta_actor_id_make(TAH_SCHED_PAGE_ENTRIES + 5u, 4u)) == NULL);
    tah_sched_remove(&sched, ta_actor_id_make(TAH_SCHED_PAGE_ENTRIES + 5u, 4u), TAH_DEAD_DESTROYED);
    CHECK(sched.known == 1u);

    /* Pages never move: an entry pointer survives growth of the directory. */
    ta_actor_id later = ta_actor_id_make(40u * TAH_SCHED_PAGE_ENTRIES, 1u);
    CHECK(tah_sched_add(&sched, later) == TA_OK);
    CHECK(tah_sched_lookup(&sched, far) == entry);

    tah_sched_remove(&sched, far, TAH_DEAD_DESTROYED);
    CHECK(tah_sched_lookup(&sched, far) == NULL);
    CHECK(sched.known == 1u);
    tah_sched_destroy(&sched);
    CHECK(tah_allocator_total(&allocator).bytes_in_use == 0);
}

typedef struct {
    uint32_t wakes;
} wake_counter;

static bool count_wake(tah_system *system, void *context) {
    (void)system;
    ((wake_counter *)context)->wakes += 1u;
    return true;
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

static ta_actor_id spawn(tah_system *system, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system->core, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    CHECK(tah_hook_actor_created(system, actor) == TA_OK);
    return actor;
}

static ta_actor_residency residency(tah_system *system, ta_actor_id actor) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    return view.residency;
}

static uint32_t count_records(const tah_system *system, tah_record_kind kind) {
    uint32_t count = 0;
    for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
        count += record->kind == kind ? 1u : 0u;
    }
    return count;
}

static void turns(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    wake_counter counter = { 0 };
    tah_system_set_driver(system, &(tah_pump_driver){ .wake = count_wake, .context = &counter });

    /* idle: a stable chart. finish: an eventless transition to a final. */
    ta_scxml_state_spec idle_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec idle = { .name = S("idle"), .states = idle_states };
    ta_str end[] = { S("end"), { 0 } };
    ta_scxml_transition_spec go[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = end }, { 0 } };
    ta_scxml_state_spec finish_states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("start"), .transitions = go },
        { .kind = TA_SCXML_STATE_FINAL, .id = S("end") },
        { 0 },
    };
    ta_scxml_spec finish = { .name = S("finish"), .states = finish_states };
    ta_definition_id idle_chart = define(system, &idle);
    ta_definition_id finish_chart = define(system, &finish);

    /* Spawning asks for one turn; more spawns before it runs do not. */
    ta_actor_id a = spawn(system, idle_chart);
    ta_actor_id b = spawn(system, finish_chart);
    CHECK(counter.wakes == 1u);
    CHECK(system->turn_pending);
    CHECK(tah_sched_lookup(&system->sched, a)->state == TAH_SCHED_RUNNABLE);

    tah_system_turn(system);
    CHECK(!system->inside_core);
    CHECK(tah_sched_lookup(&system->sched, a)->state == TAH_SCHED_IDLE);
    CHECK(residency(system, a) == TA_ACTOR_IDLE);
    CHECK(residency(system, b) == TA_ACTOR_DONE);
    CHECK(count_records(system, TAH_RECORD_DONE) == 1u);
    CHECK(system->sched.records->actor == b);
    CHECK((tah_sched_lookup(&system->sched, b)->flags & TAH_SCHED_FINISHED) != 0);
    tah_system_end_turn(system);
    CHECK(tah_system_quiet(system));
    CHECK(!system->turn_pending);
    CHECK(counter.wakes == 1u);

    /* A destroyed actor's queue entry is skipped. */
    ta_actor_id c = spawn(system, idle_chart);
    CHECK(ta_actor_destroy(system->core, c) == TA_OK);
    CHECK(tah_hook_actor_removed(system, c) == TA_OK);
    tah_system_turn(system);
    CHECK(system->sched.record_count == 0u);
    tah_system_end_turn(system);
    CHECK(tah_system_quiet(system));

    /* A slot reused by a new actor: the old ID's entry is gone. */
    ta_actor_id d = spawn(system, idle_chart);
    CHECK(ta_actor_id_slot(d) == ta_actor_id_slot(c));
    CHECK(tah_sched_lookup(&system->sched, c) == NULL);
    tah_system_turn(system);
    tah_system_end_turn(system);
    CHECK(residency(system, d) == TA_ACTOR_IDLE);

    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

/* A long entry handler, split by the action budget over several turns. */
static void action_budget(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    enum { ACTIONS = 40 };
    ta_scxml_action_spec logs[ACTIONS + 1];
    memset(logs, 0, sizeof logs);
    for (int index = 0; index < ACTIONS; index += 1) {
        logs[index] = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = S("step") } };
    }
    ta_scxml_handler_spec onentry[] = { { .actions = logs }, { 0 } };
    /* The bootstrap never pauses (core ADR 0061), so the long handler is on a
     * state entered after it, through an eventless transition. */
    ta_str busy_target[] = { S("busy"), { 0 } };
    ta_scxml_transition_spec go[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = busy_target }, { 0 } };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("start"), .transitions = go },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("busy"), .onentry = onentry },
        { 0 },
    };
    ta_scxml_spec spec = { .name = S("busy"), .states = states };
    ta_definition_id chart = define(system, &spec);
    ta_scxml_state_spec idle_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec idle = { .name = S("idle"), .states = idle_states };
    ta_definition_id idle_chart = define(system, &idle);

    system->budgets.actions = 8u;
    ta_actor_id busy = spawn(system, chart);
    ta_actor_id other = 0;
    uint32_t turns = 0;
    while (tah_sched_lookup(&system->sched, busy)->state == TAH_SCHED_RUNNABLE) {
        tah_system_turn(system);
        tah_system_end_turn(system);
        turns += 1u;
        if (turns == 1u) {
            /* Another actor is spawned while the busy one is paused. Core runs one input at a time (core ADR 0099
             * section 4; ADR 0103 adds slots), so its bootstrap waits until the paused macrostep ends. */
            other = spawn(system, idle_chart);
            tah_system_turn(system);
            tah_system_end_turn(system);
            turns += 1u;
            CHECK(residency(system, other) == TA_ACTOR_RUNNING);
            CHECK(tah_sched_lookup(&system->sched, busy)->state == TAH_SCHED_RUNNABLE);
        }
        CHECK(turns < 100u);
    }
    /* A turn makes one ta_run call for each runnable actor, and each call continues the paused macrostep, so
     * the 40 units, 8 to a call, take more than one turn. */
    CHECK(turns >= 3u);
    CHECK(residency(system, busy) == TA_ACTOR_IDLE);
    while (!tah_system_quiet(system)) {
        tah_system_turn(system);
        tah_system_end_turn(system);
        turns += 1u;
        CHECK(turns < 200u);
    }
    CHECK(residency(system, other) == TA_ACTOR_IDLE); /* it bootstrapped after the busy one */

    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

/* How many of `actors` the scheduler has settled (they are no longer RUNNABLE). */
static uint32_t settled_count(tah_system *system, const ta_actor_id *actors, uint32_t count) {
    uint32_t settled = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        tah_sched_entry *entry = tah_sched_lookup(&system->sched, actors[index]);
        settled += entry != NULL && entry->state != TAH_SCHED_RUNNABLE ? 1u : 0u;
    }
    return settled;
}

/* The resume limit bounds the inputs one turn completes; the rest waits for the next. A ta_run() call serves
 * whichever actors' inputs are next, so the queue keeps entries of actors the call already settled, and the
 * count of settled actors is what the limit bounds. */
static void resume_limit(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    ta_scxml_state_spec idle_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec idle = { .name = S("idle"), .states = idle_states };
    ta_definition_id chart = define(system, &idle);
    system->budgets.resumes = 10u;
    enum { COUNT = 25 };
    ta_actor_id actors[COUNT];
    for (int index = 0; index < COUNT; index += 1) {
        actors[index] = spawn(system, chart);
    }
    CHECK(tah_ring_count(&system->sched.runnable) == COUNT);
    CHECK(settled_count(system, actors, COUNT) == 0);
    tah_system_turn(system);
    tah_system_end_turn(system);
    /* The turn's 10 microsteps: nine bootstraps end, and the tenth is stopped inside its macrostep, which the next
     * turn continues first. */
    CHECK(settled_count(system, actors, COUNT) == 9);
    CHECK(system->turn_pending); /* work remains: another turn was asked for */
    tah_system_turn(system);
    tah_system_turn(system);
    tah_system_end_turn(system);
    CHECK(tah_system_quiet(system));
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

/* turn_work_is_bounded checks that a turn costs what its resume budget allows, not the length of the runnable
 * queue: after the budget is used the turn stops and leaves the queue as it is. Counts, not clocks: the ring after
 * one turn still holds nearly every actor, and bootstrapping N actors takes about N / resumes turns, none of which
 * pushes an actor twice. */
static void turn_work_is_bounded(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    ta_scxml_state_spec idle_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec idle = { .name = S("idle"), .states = idle_states };
    ta_definition_id chart = define(system, &idle);
    system->budgets.resumes = 10u;
    enum { COUNT = 2000 };
    static ta_actor_id actors[COUNT];
    for (int index = 0; index < COUNT; index += 1) {
        actors[index] = spawn(system, chart);
    }
    CHECK(tah_ring_count(&system->sched.runnable) == COUNT);
    tah_system_turn(system);
    tah_system_end_turn(system);
    CHECK(settled_count(system, actors, COUNT) == 9);
    /* The turn popped the entry it ran and the few it settled, not the 2000 behind them. */
    CHECK(system->sched.turn_visited <= 2u * system->budgets.resumes);
    CHECK(tah_ring_count(&system->sched.runnable) >= COUNT - 2u * system->budgets.resumes);
    uint32_t turns = 1;
    while (!tah_system_quiet(system) && turns < COUNT / 8u + 8u) {
        tah_system_turn(system);
        tah_system_end_turn(system);
        turns += 1u;
        CHECK(system->sched.turn_visited <= 2u * system->budgets.resumes);
        CHECK(tah_ring_count(&system->sched.runnable) <= COUNT);
    }
    CHECK(tah_system_quiet(system));
    CHECK(turns < COUNT / 8u + 8u);
    CHECK(settled_count(system, actors, COUNT) == COUNT);
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

/* run_until_quiet takes turns until the kernel is quiet and returns how many it took. `limit` turns without
 * quiet is a failure the check reports. */
static uint32_t run_until_quiet(tah_system *system, uint32_t limit) {
    uint32_t turns = 0;
    while (!tah_system_quiet(system) && turns < limit) {
        tah_system_turn(system);
        tah_system_end_turn(system);
        turns += 1u;
    }
    CHECK(tah_system_quiet(system));
    return turns;
}

/* work_counts is what work_per_actor measured: ta_run() calls, not time. */
typedef struct {
    uint64_t bootstrap_calls;
    uint64_t delivery_calls;
    uint32_t turns;
} work_counts;

/* work_per_actor spawns `count` idle actors, counts the ta_run() calls that bootstrap them all, then posts
 * `per_actor` messages to each of `posted` of them and counts the calls that deliver those. These are what a
 * million-actor run pays per actor and per message: bootstrap makes about one call per actor and delivery about
 * one per message, and neither may depend on how many other actors exist. */
static work_counts work_per_actor(uint32_t count, uint32_t posted, uint32_t per_actor) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    ta_scxml_state_spec idle_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec idle = { .name = S("idle"), .states = idle_states };
    ta_definition_id chart = define(system, &idle);
    system->budgets.resumes = 10u;
    static ta_actor_id actors[4096];
    CHECK(count <= sizeof actors / sizeof actors[0]);
    for (uint32_t index = 0; index < count; index += 1u) {
        actors[index] = spawn(system, chart);
    }
    work_counts counts = { .turns = run_until_quiet(system, count) };
    CHECK(settled_count(system, actors, count) == count);
    counts.bootstrap_calls = system->sched.run_calls_total;
    CHECK(counts.bootstrap_calls > 0);

    ta_event_name_id ping = 0;
    CHECK(tah_event_name_intern(system->core, S("ping"), &ping) == TA_OK);
    ta_event event = { .name = ping, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    for (uint32_t round = 0; round < per_actor; round += 1u) {
        for (uint32_t index = 0; index < posted; index += 1u) {
            ta_actor_id target = actors[index * (count / posted)];
            CHECK(tah_system_post(system, &(tah_message){ .target = target, .event = &event }) == TA_OK);
        }
    }
    (void)run_until_quiet(system, count + posted * per_actor);
    counts.delivery_calls = system->sched.run_calls_total - counts.bootstrap_calls;
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
    return counts;
}

/* bootstrap_and_delivery_cost_a_constant_number_of_calls compares two populations: the ta_run() calls per
 * bootstrapped actor and per delivered message must be the same at 512 and at 2048 actors. A pass over the
 * population, a rebuilt table or a re-queue per call would make the larger one cost more per unit, which is what
 * scale.js's ratio bound sees as a regression. */
static void bootstrap_and_delivery_cost_a_constant_number_of_calls(void) {
    enum { POSTED = 64, EACH = 8, SMALL = 512, LARGE = 2048 };
    work_counts small = work_per_actor(SMALL, POSTED, EACH);
    work_counts large = work_per_actor(LARGE, POSTED, EACH);
    /* Bootstrap: at most about one call per actor, at the same rate at both sizes (one call in a hundred of slack). */
    CHECK(small.bootstrap_calls <= SMALL + small.turns);
    CHECK(large.bootstrap_calls <= LARGE + large.turns);
    CHECK(large.bootstrap_calls * SMALL <= (small.bootstrap_calls + SMALL / 100u) * LARGE);
    /* Delivery: the calls for POSTED * EACH messages do not depend on the population. */
    CHECK(small.delivery_calls <= POSTED * EACH + 2u * POSTED);
    CHECK(large.delivery_calls <= small.delivery_calls + 2u);
    CHECK(small.delivery_calls <= large.delivery_calls + 2u);
}

int main(void) {
    table_pages();
    turns();
    action_budget();
    resume_limit();
    turn_work_is_bounded();
    bootstrap_and_delivery_cost_a_constant_number_of_calls();
    puts("scheduler_test passed");
    return 0;
}
