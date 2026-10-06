/*
 * The turn (ADRs 0005, 0007, 0008), driven without Node-API: a turn runs
 * what core holds, bootstraps and done records, budget pauses, the bound on
 * the inputs one turn finishes, and the cost of bootstrap and delivery in
 * calls of ta_run(), not clocks. The kernel keeps no table with an entry per
 * actor and no queue of its own: core's run queue and started queue are the
 * only queues.
 */
#include "check.h"
#include <string.h>

#include "tinyactors_host/hooks.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "scheduler_private.h"
#include "system_private.h"

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })

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
    CHECK(ta_run_pending(system->core));

    tah_system_turn(system);
    CHECK(!system->inside_core);
    CHECK(residency(system, a) == TA_ACTOR_IDLE);
    CHECK(residency(system, b) == TA_ACTOR_DONE);
    CHECK(count_records(system, TAH_RECORD_DONE) == 1u);
    CHECK(system->sched.records->actor == b);
    tah_system_end_turn(system);
    CHECK(tah_system_quiet(system));
    CHECK(!system->turn_pending);
    CHECK(counter.wakes == 1u);

    /* A destroyed actor's bootstrap is dropped. */
    ta_actor_id c = spawn(system, idle_chart);
    CHECK(ta_actor_destroy(system->core, c) == TA_OK);
    CHECK(tah_hook_actor_removed(system, c) == TA_OK);
    tah_system_turn(system);
    CHECK(system->sched.record_count == 0u);
    tah_system_end_turn(system);
    CHECK(tah_system_quiet(system));

    /* A slot reused by a new actor. */
    ta_actor_id d = spawn(system, idle_chart);
    CHECK(ta_actor_id_slot(d) == ta_actor_id_slot(c));
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
    while (residency(system, busy) == TA_ACTOR_RUNNING) {
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
            CHECK(residency(system, busy) == TA_ACTOR_RUNNING);
        }
        CHECK(turns < 100u);
    }
    /* Each turn's call continues the paused macrostep, so the 40 units, 8 to a call, take more than one turn. */
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

/* How many of `actors` have finished their bootstrap (they are idle). */
static uint32_t settled_count(tah_system *system, const ta_actor_id *actors, uint32_t count) {
    uint32_t settled = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        settled += residency(system, actors[index]) == TA_ACTOR_IDLE ? 1u : 0u;
    }
    return settled;
}

/* The input limit bounds the inputs one turn finishes; the rest waits for the next. */
static void input_limit(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    ta_scxml_state_spec idle_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec idle = { .name = S("idle"), .states = idle_states };
    ta_definition_id chart = define(system, &idle);
    system->budgets.inputs = 10u;
    enum { COUNT = 25 };
    ta_actor_id actors[COUNT];
    for (int index = 0; index < COUNT; index += 1) {
        actors[index] = spawn(system, chart);
    }
    CHECK(settled_count(system, actors, COUNT) == 0);
    tah_system_turn(system);
    tah_system_end_turn(system);
    uint32_t first = settled_count(system, actors, COUNT);
    CHECK(first == 10u); /* the turn asks for more after each input: it stops at the bound */
    CHECK(system->turn_pending); /* work remains: another turn was asked for */
    tah_system_turn(system);
    tah_system_end_turn(system);
    CHECK(settled_count(system, actors, COUNT) > first);
    while (!tah_system_quiet(system)) {
        tah_system_turn(system);
        tah_system_end_turn(system);
    }
    CHECK(settled_count(system, actors, COUNT) == COUNT);
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

/* A turn costs what its input bound allows, not the number of actors: every turn of a large population finishes
 * the same number of inputs, in a bounded number of ta_run() calls. Counts, not clocks. */
static void turn_work_is_bounded(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    ta_scxml_state_spec idle_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec idle = { .name = S("idle"), .states = idle_states };
    ta_definition_id chart = define(system, &idle);
    system->budgets.inputs = 10u;
    enum { COUNT = 2000 };
    static ta_actor_id actors[COUNT];
    for (int index = 0; index < COUNT; index += 1) {
        actors[index] = spawn(system, chart);
    }
    tah_system_turn(system);
    tah_system_end_turn(system);
    CHECK(settled_count(system, actors, COUNT) == 10u);
    CHECK(system->sched.turn_run_calls <= 10u + 1u); /* one call for each input, as the staging bound asks */
    uint32_t turns = 1;
    while (!tah_system_quiet(system) && turns < COUNT / 10u + 8u) {
        tah_system_turn(system);
        tah_system_end_turn(system);
        turns += 1u;
        CHECK(system->sched.turn_run_calls <= 10u + 1u);
    }
    CHECK(tah_system_quiet(system));
    CHECK(turns <= COUNT / 10u + 1u);
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
    system->budgets.inputs = 10u;
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
    turns();
    action_budget();
    input_limit();
    turn_work_is_bounded();
    bootstrap_and_delivery_cost_a_constant_number_of_calls();
    puts("scheduler_test passed");
    return 0;
}
