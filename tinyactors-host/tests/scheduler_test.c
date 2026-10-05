/*
 * The scheduling table and the run part of a turn (ADRs 0005, 0007, 0008;
 * core ADR 0103), driven without Node-API: entries in fixed pages, a
 * destroyed actor's bootstrap dropped, coalesced wake-ups, bootstrap,
 * budget pauses that hold the one slot, the turn's call budget, done
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

    /* A destroyed actor's bootstrap is dropped. */
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

/*
 * A long entry handler, split by the action budget over several turns: a
 * paused macrostep has one slice a turn. With one slot, the paused
 * macrostep goes on before anything else is taken: another actor's
 * bootstrap waits for it (core ADR 0103).
 */
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
            /* Another actor's bootstrap waits behind the paused macrostep. */
            other = spawn(system, idle_chart);
            tah_system_turn(system);
            tah_system_end_turn(system);
            turns += 1u;
            CHECK(residency(system, other) == TA_ACTOR_RUNNING);
            CHECK(tah_sched_lookup(&system->sched, busy)->state == TAH_SCHED_RUNNABLE);
        }
        CHECK(turns < 100u);
    }
    CHECK(turns >= ACTIONS / 8);
    CHECK(residency(system, busy) == TA_ACTOR_IDLE);
    while (!tah_system_quiet(system)) {
        tah_system_turn(system);
        tah_system_end_turn(system);
        CHECK(turns < 100u);
        turns += 1u;
    }
    CHECK(residency(system, other) == TA_ACTOR_IDLE);

    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

/* The resume limit bounds one turn: each ta_run() call takes one input, so
 * ten calls bootstrap ten actors; the rest waits for the next turn. */
static void resume_limit(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    ta_scxml_state_spec idle_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec idle = { .name = S("idle"), .states = idle_states };
    ta_definition_id chart = define(system, &idle);
    system->budgets.resumes = 10u;
    enum { COUNT = 25 };
    for (int index = 0; index < COUNT; index += 1) {
        spawn(system, chart);
    }
    tah_sched_stats stats;
    tah_sched_read_stats(&system->sched, &stats);
    CHECK(stats.runnable == COUNT);
    tah_system_turn(system);
    tah_system_end_turn(system);
    tah_sched_read_stats(&system->sched, &stats);
    CHECK(stats.runnable == COUNT - 10);
    CHECK(system->turn_pending); /* work remains: another turn was asked for */
    for (int index = 0; index < 10 && !tah_system_quiet(system); index += 1) {
        tah_system_turn(system);
        tah_system_end_turn(system);
    }
    CHECK(tah_system_quiet(system));
    tah_sched_read_stats(&system->sched, &stats);
    CHECK(stats.runnable == 0);
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

int main(void) {
    table_pages();
    turns();
    action_budget();
    resume_limit();
    puts("scheduler_test passed");
    return 0;
}
