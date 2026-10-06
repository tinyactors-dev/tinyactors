/*
 * The run (src/run.h): inputs admitted into the system's pool, and
 * ta_run(), which takes them in arrival order with one macrostep slot and
 * reports what became of each (ADRs 0099, 0103).
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which can refuse any allocation and counts
 * the blocks that are live.
 *
 * Claims proven here, in order:
 *
 * 1. admission runs nothing: an admitted event waits until ta_run(), which
 *    takes a spawn's bootstrap before it (correlation 0), reports each
 *    input COMPLETED, IGNORED or FINISHED with the session and its
 *    correlation, and answers TA_RUN_IDLE when nothing is left;
 * 2. a full pool bounces: at the admission watermark of the pool, or of
 *    the name area, ta_actor_deliver() answers TA_ERR_FULL and retains
 *    nothing, and a spawn answers TA_ERR_FULL and makes nothing; the rest
 *    of the pool stays free; after a run gives envelopes back, admission
 *    takes again;
 * 3. order is kept per sender and receiver: inputs for several sessions,
 *    admitted interleaved, are taken in arrival order, and each session
 *    takes its own in the order they were admitted;
 * 4. outcomes report each input once: with `report_taken`, every input is
 *    TAKEN once and then ends once; a buffer of one outcome answers
 *    TA_RUN_MORE and loses nothing, and the call that empties the run
 *    queue answers TA_RUN_IDLE; a buffer with no room for what one input
 *    reports answers TA_ERR_INVALID and takes nothing;
 * 5. the budgets are the call's: a budget that runs out inside a
 *    macrostep answers TA_RUN_PAUSED, and the next call continues it
 *    before it takes the next input, which it then takes with what is
 *    left; an action budget pauses inside a microstep, and one spent
 *    exactly by an input answers TA_RUN_MORE between inputs;
 * 6. a fault stops the session, not the run: a runaway is reported
 *    FAULTED with TA_ERR_RUNAWAY, other sessions' inputs run, and the
 *    stopped session's wait on its deferred list in order;
 *    ta_actor_release() lets it go on without the microstep limit, its
 *    input is reported again when it ends, and its deferred inputs follow
 *    in order; releasing a session no fault stopped answers
 *    TA_ERR_INVALID, an unknown one TA_ERR_NOT_FOUND; the next macrostep
 *    has the limit again;
 * 7. an input that cannot be taken is refused once: its session gone
 *    (TA_ERR_NOT_FOUND, including the inputs deferred for it), finished
 *    (TA_ERR_DONE), or an operator transition whose source is no longer
 *    active (TA_ERR_INVALID); a destroyed session's bootstrap is dropped
 *    unreported;
 * 8. ta_system_withdraw_input() drops an admitted input that is not taken,
 *    on the run queue or a deferred list, which is never reported;
 *    answers TA_ERR_NOT_FOUND for one taken, or none, and TA_ERR_INVALID
 *    for correlation 0;
 * 9. the session filter runs one session's inputs, the first queued for
 *    it first, and leaves the others where they are, a macrostep paused
 *    in the slot included;
 * 10. an invocation's error report is admitted like an event: its event
 *    is queued and the invocation retired when it is taken, and a report
 *    whose invocation ended before then is refused with TA_ERR_NOT_FOUND;
 * 11. ta_run() refuses bad parameters with TA_ERR_INVALID, and a call
 *    from inside a ta_run() with TA_ERR_BUSY, running nothing;
 * 12. running out of memory: a refill that fails leaves admission what is
 *    free, and once nothing is free admission answers TA_ERR_NOMEM, not
 *    TA_ERR_FULL, far below the watermark; taking an event allocates
 *    nothing, so a run takes and runs admitted events with the allocator
 *    refusing everything; a run that cannot take the next input (a
 *    report whose event finds no room in its session's internal queue,
 *    while another session's paused macrostep holds the system's spare
 *    queue) answers TA_ERR_NOMEM and the input waits at the head, taken by
 *    the next run; nothing leaks.
 */
#include "../include/tinyactors.c"
#include "test_drive.h"
#include "test_allocator.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                                 \
        }                                                                                  \
    } while (0)

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define END { 0 }

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * Charts
 */

/* s0 --e1--> s1 --e2--> s2 --e3--> s3: only the events in order advance it. */
static const ta_str to_s1[] = { STR("s1"), END };
static const ta_str to_s2[] = { STR("s2"), END };
static const ta_str to_s3[] = { STR("s3"), END };
static const ta_str to_end[] = { STR("end"), END };
static const ta_scxml_transition_spec s0_out[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e1"), .targets = to_s1 },
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("stop"), .targets = to_end },
    END,
};
static const ta_scxml_transition_spec s1_out[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e2"), .targets = to_s2 },
    END,
};
static const ta_scxml_transition_spec s2_out[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e3"), .targets = to_s3 },
    END,
};
static const ta_scxml_state_spec sequence_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s0"), .transitions = s0_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s1"), .transitions = s1_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s2"), .transitions = s2_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s3") },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("end") },
    END,
};
static const ta_scxml_spec sequence = { .name = STR("sequence"), .states = sequence_states };

/* idle --long--> c1 -> c2 -> ... -> c8 -> idle: nine microsteps in one
 * macrostep, eventless after the first. */
static const ta_str to_c1[] = { STR("c1"), END };
static const ta_str to_c2[] = { STR("c2"), END };
static const ta_str to_c3[] = { STR("c3"), END };
static const ta_str to_c4[] = { STR("c4"), END };
static const ta_str to_c5[] = { STR("c5"), END };
static const ta_str to_c6[] = { STR("c6"), END };
static const ta_str to_c7[] = { STR("c7"), END };
static const ta_str to_c8[] = { STR("c8"), END };
static const ta_str to_idle[] = { STR("idle"), END };
#define STEP(name, target) \
    static const ta_scxml_transition_spec name[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = target }, END }
STEP(c1_out, to_c2);
STEP(c2_out, to_c3);
STEP(c3_out, to_c4);
STEP(c4_out, to_c5);
STEP(c5_out, to_c6);
STEP(c6_out, to_c7);
STEP(c7_out, to_c8);
STEP(c8_out, to_idle);
static const ta_scxml_action_spec two_logs[] = {
    { .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = STR("one") } },
    { .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = STR("two") } },
    END,
};
static const ta_scxml_handler_spec c1_entry[] = { { .actions = two_logs }, END };
static const ta_scxml_transition_spec idle_out[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("long"), .targets = to_c1 },
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e1"), .targets = to_s1 },
    END,
};
static const ta_scxml_state_spec chain_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c1"), .onentry = c1_entry, .transitions = c1_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c2"), .transitions = c2_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c3"), .transitions = c3_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c4"), .transitions = c4_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c5"), .transitions = c5_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c6"), .transitions = c6_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c7"), .transitions = c7_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c8"), .transitions = c8_out },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s1") },
    END,
};
static const ta_scxml_spec chain = { .name = STR("chain"), .states = chain_states };

/* busy invokes a service of type "svc"; its error event takes it to failed. */
static const ta_str to_failed[] = { STR("failed"), END };
static const ta_scxml_transition_spec busy_out[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.communication"), .targets = to_failed },
    END,
};
static const ta_scxml_invoke_spec busy_invokes[] = {
    { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") },
      .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("job") } },
    END,
};
static const ta_scxml_state_spec invoking_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("busy"), .transitions = busy_out, .invokes = busy_invokes },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("failed") },
    END,
};
static const ta_scxml_spec invoking = { .name = STR("invoking"), .states = invoking_states };

/* ------------------------------------------------------------------------
 * A system, its actors and its runs
 */

typedef struct {
    ta_system *system;
    ta_definition_id sequence;
    ta_definition_id chain;
    ta_input_outcome outcomes[64];
    uint32_t count;
    ta_run_status status;
} fixture;

static ta_definition_id define(ta_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = spec,
                                                                             .datamodel = ta_null_datamodel(),
                                                                             .allocator = allocator,
                                                                             .out = &definition }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static fixture open_with(const ta_param_value *params, uint32_t count) {
    fixture f = { 0 };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params, .param_count = count },
                           &f.system) == TA_OK);
    f.sequence = define(f.system, &sequence);
    f.chain = define(f.system, &chain);
    return f;
}

static fixture open_default(void) {
    return open_with(NULL, 0);
}

static void close_fixture(fixture *f) {
    ta_system_destroy(f->system);
    CHECK(heap.live == 0);
}

static ta_actor_id spawn(fixture *f, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(f->system, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    return actor;
}

static ta_event_name_id name_of(fixture *f, const char *text) {
    ta_event_name_id name = 0;
    CHECK(ta_trie_intern_dotted(&f->system->event_names, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) },
                                &name) == TA_OK);
    return name;
}

static ta_result deliver(fixture *f, ta_actor_id actor, const char *text, uint64_t correlation) {
    ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(f->system, &(ta_actor_deliver_params){ .actor = actor,
                                                                   .name = { .ptr = text, .len = (uint32_t)strlen(text) },
                                                                   .event = &event,
                                                                   .time = ta_system_time(f->system),
                                                                   .correlation = correlation });
}

/* What one ta_run() call may do. */
typedef struct {
    uint32_t budget;
    uint32_t action_budget;
    uint32_t capacity;
    ta_actor_id session;
    bool report_taken;
} run_limits;

/* One ta_run() call; its outcomes replace the fixture's. */
static ta_result run_with(fixture *f, run_limits limits) {
    f->count = 0;
    return ta_run(f->system, &(ta_run_params){ .budget = limits.budget != 0 ? limits.budget : 1000u,
                                                .action_budget = limits.action_budget,
                                                .session = limits.session,
                                                .report_taken = limits.report_taken,
                                                .outcomes = f->outcomes,
                                                .outcome_capacity = limits.capacity != 0 ? limits.capacity : 64u,
                                                .outcome_count = &f->count,
                                                .status = &f->status });
}

static ta_result run_all(fixture *f) {
    return run_with(f, (run_limits){ 0 });
}

/* Whether outcome `at` is `kind` for `actor` with `correlation`. */
static bool outcome_is(const fixture *f, uint32_t at, ta_input_outcome_kind kind, ta_actor_id actor,
                       uint64_t correlation) {
    return at < f->count && f->outcomes[at].kind == kind && f->outcomes[at].actor == actor &&
           f->outcomes[at].correlation == correlation;
}

static bool in_state(fixture *f, ta_actor_id actor, const char *id) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f->system, actor, &view) == TA_OK);
    ta_state_id wanted = 0;
    if (ta_definition_find_state(f->system, &(ta_definition_find_state_params){
                                                .definition = view.definition_id,
                                                .id = { .ptr = id, .len = (uint32_t)strlen(id) },
                                                .out = &wanted }) != TA_OK) {
        return false;
    }
    ta_state_id active[8];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(f->system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                             .capacity = 8, .count = &count }) ==
          TA_OK);
    return count == 1 && active[0] == wanted;
}

/* A value that counts its references. */
typedef struct {
    long references;
} counted;

static void counted_retain(void *self) {
    ((counted *)self)->references += 1;
}

static void counted_release(void *self) {
    ((counted *)self)->references -= 1;
}

static const ta_value_vtable counted_vtable = { .retain = counted_retain, .release = counted_release };

/* ------------------------------------------------------------------------
 * Tests
 */

/* Claim 1. */
static void test_admission_runs_nothing(void) {
    fixture f = open_default();
    ta_actor_id actor = spawn(&f, f.sequence);
    CHECK(deliver(&f, actor, "e1", 11) == TA_OK);
    CHECK(deliver(&f, actor, "noise", 12) == TA_OK);
    CHECK(deliver(&f, actor, "e2", 13) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, actor, &view) == TA_OK);
    CHECK(view.macrostep_in_progress && view.current_event == NULL); /* the bootstrap waits; no event is held */
    CHECK(run_all(&f) == TA_OK);
    CHECK(f.count == 4 && f.status == TA_RUN_IDLE);
    CHECK(outcome_is(&f, 0, TA_INPUT_COMPLETED, actor, 0)); /* the bootstrap */
    CHECK(outcome_is(&f, 1, TA_INPUT_COMPLETED, actor, 11));
    CHECK(outcome_is(&f, 2, TA_INPUT_IGNORED, actor, 12));
    CHECK(outcome_is(&f, 3, TA_INPUT_COMPLETED, actor, 13));
    CHECK(f.outcomes[1].code == TA_OK);
    CHECK(in_state(&f, actor, "s2"));
    CHECK(run_all(&f) == TA_OK && f.count == 0 && f.status == TA_RUN_IDLE);
    CHECK(deliver(&f, actor, "e3", 14) == TA_OK);
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_COMPLETED, actor, 14));

    ta_actor_id other = spawn(&f, f.sequence);
    CHECK(run_all(&f) == TA_OK && f.count == 1);
    CHECK(deliver(&f, other, "stop", 15) == TA_OK);
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_FINISHED, other, 15));
    close_fixture(&f);
}

/* Claim 2. */
static void test_full_pool_bounces(void) {
    /* 8 envelopes, 4 of them for admission. */
    ta_param_value small_pool[] = { { TA_PARAM_POOL_ENVELOPES, 8 }, { TA_PARAM_ADMISSION_WATERMARK, 50 } };
    fixture f = open_with(small_pool, 2);
    ta_actor_id actor = spawn(&f, f.sequence);
    CHECK(run_all(&f) == TA_OK);
    counted value = { .references = 1 };
    ta_event event = { .name = name_of(&f, "noise"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE,
                       .data = { .self = &value, .v = &counted_vtable } };
    for (uint64_t correlation = 1; correlation <= 4; correlation += 1u) {
        CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .actor = actor, .name = test_event_text(f.system, &event), .event = &event,
                                                                     .time = ta_system_time(f.system),
                                                                     .correlation = correlation }) == TA_OK);
    }
    CHECK(value.references == 5);
    long live = heap.live;
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .actor = actor, .name = test_event_text(f.system, &event), .event = &event,
                                                                 .time = ta_system_time(f.system),
                                                                 .correlation = 5 }) == TA_ERR_FULL);
    CHECK(value.references == 5 && heap.live == live);
    ta_actor_id refused = 77;
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.sequence, .out = &refused }) ==
          TA_ERR_FULL);
    CHECK(refused == 77 && heap.live == live);
    /* The rest of the pool is free: the run takes what was admitted. */
    CHECK(f.system->run.pool.quota.used == 4u);
    CHECK(run_all(&f) == TA_OK && f.count == 4 && f.status == TA_RUN_IDLE);
    CHECK(value.references == 1);
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .actor = actor, .name = test_event_text(f.system, &event), .event = &event,
                                                                 .time = ta_system_time(f.system),
                                                                 .correlation = 6 }) == TA_OK);
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_IGNORED, actor, 6));
    CHECK(value.references == 1);
    close_fixture(&f);

    /* The name area's watermark: 4,096 bytes are 64 chunks, 32 for admission. */
    ta_param_value small_names[] = { { TA_PARAM_NAME_AREA_BYTES, 4096 } };
    f = open_with(small_names, 1);
    actor = spawn(&f, f.sequence);
    CHECK(run_all(&f) == TA_OK);
    uint32_t admitted = 0;
    while (deliver(&f, actor, "noise", admitted + 1u) == TA_OK && admitted < 100u) {
        admitted += 1u;
    }
    CHECK(admitted == 32u);
    CHECK(f.system->run.pool.quota.used == 32u); /* the pool has room: the names are full */
    CHECK(run_all(&f) == TA_OK && f.count == 32);
    CHECK(deliver(&f, actor, "noise", 99) == TA_OK);
    CHECK(run_all(&f) == TA_OK && f.count == 1);
    close_fixture(&f);
}

/* Claim 3. */
static void test_order_per_sender_and_receiver(void) {
    fixture f = open_default();
    ta_actor_id a = spawn(&f, f.sequence);
    ta_actor_id b = spawn(&f, f.sequence);
    ta_actor_id c = spawn(&f, f.chain);
    CHECK(run_all(&f) == TA_OK && f.count == 3);
    static const char *const order[] = { "e1", "e1", "e2", "e1", "e2", "e3", "e3" };
    const ta_actor_id targets[] = { a, b, a, c, b, a, b };
    for (uint32_t index = 0; index < 7u; index += 1u) {
        CHECK(deliver(&f, targets[index], order[index], index + 1u) == TA_OK);
    }
    CHECK(run_all(&f) == TA_OK && f.count == 7);
    for (uint32_t index = 0; index < 7u; index += 1u) {
        CHECK(f.outcomes[index].correlation == index + 1u && f.outcomes[index].actor == targets[index]);
    }
    CHECK(in_state(&f, a, "s3") && in_state(&f, b, "s3") && in_state(&f, c, "s1"));
    close_fixture(&f);
}

/* Claim 4. */
static void test_outcomes_once(void) {
    fixture f = open_default();
    ta_actor_id a = spawn(&f, f.sequence);
    ta_actor_id b = spawn(&f, f.sequence);
    CHECK(deliver(&f, a, "e1", 1) == TA_OK);
    CHECK(deliver(&f, b, "noise", 2) == TA_OK);
    CHECK(deliver(&f, a, "stop", 3) == TA_OK); /* ignored in s1 */
    CHECK(deliver(&f, b, "stop", 4) == TA_OK);
    CHECK(deliver(&f, b, "e1", 5) == TA_OK); /* b finished: refused */
    uint32_t taken[8] = { 0 };
    uint32_t ended[8] = { 0 };
    uint32_t calls = 0;
    ta_input_outcome_kind kinds[8] = { 0 };
    for (;;) {
        CHECK(run_with(&f, (run_limits){ .capacity = 2, .report_taken = true }) == TA_OK);
        calls += 1u;
        for (uint32_t at = 0; at < f.count; at += 1u) {
            uint64_t correlation = f.outcomes[at].correlation;
            CHECK(correlation < 8u);
            if (f.outcomes[at].kind == TA_INPUT_TAKEN) {
                CHECK(correlation == 0u || ended[correlation] == 0u); /* taken before it ends */
                taken[correlation] += 1u;
            } else {
                ended[correlation] += 1u;
                kinds[correlation] = f.outcomes[at].kind;
            }
        }
        if (f.status == TA_RUN_IDLE || calls > 20u) {
            break;
        }
        CHECK(f.status == TA_RUN_MORE);
    }
    CHECK(f.status == TA_RUN_IDLE && calls == 7u);
    CHECK(taken[0] == 2u && ended[0] == 2u); /* the two bootstraps */
    for (uint32_t correlation = 1; correlation <= 4u; correlation += 1u) {
        CHECK(taken[correlation] == 1u && ended[correlation] == 1u);
    }
    CHECK(taken[5] == 0u && ended[5] == 1u && kinds[5] == TA_INPUT_REFUSED);
    CHECK(kinds[1] == TA_INPUT_COMPLETED && kinds[2] == TA_INPUT_IGNORED && kinds[3] == TA_INPUT_IGNORED &&
          kinds[4] == TA_INPUT_FINISHED);

    /* A buffer of one: MORE after each input, IDLE once the queue is empty. */
    CHECK(deliver(&f, a, "e2", 6) == TA_OK);
    CHECK(deliver(&f, a, "e3", 7) == TA_OK);
    CHECK(run_with(&f, (run_limits){ .capacity = 1 }) == TA_OK);
    CHECK(f.count == 1 && f.status == TA_RUN_MORE && outcome_is(&f, 0, TA_INPUT_COMPLETED, a, 6));
    CHECK(run_with(&f, (run_limits){ .capacity = 1 }) == TA_OK);
    CHECK(f.count == 1 && f.status == TA_RUN_IDLE && outcome_is(&f, 0, TA_INPUT_COMPLETED, a, 7));
    /* No room for what one input reports is a caller's error, and takes nothing. */
    CHECK(deliver(&f, a, "noise", 8) == TA_OK);
    CHECK(ta_run(f.system, &(ta_run_params){ .budget = 10, .outcome_count = &f.count, .status = &f.status }) ==
          TA_ERR_INVALID);
    CHECK(run_with(&f, (run_limits){ .capacity = 1, .report_taken = true }) == TA_ERR_INVALID);
    CHECK(f.count == 0);
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_IGNORED, a, 8));
    close_fixture(&f);
}

/* Claim 5. */
static void test_budgets(void) {
    fixture f = open_default();
    ta_actor_id c = spawn(&f, f.chain);
    ta_actor_id s = spawn(&f, f.sequence);
    CHECK(run_all(&f) == TA_OK && f.count == 2);
    CHECK(deliver(&f, c, "long", 1) == TA_OK);
    CHECK(deliver(&f, s, "e1", 2) == TA_OK);
    /* Nine microsteps: three calls of four pause twice, then the chain ends. */
    CHECK(run_with(&f, (run_limits){ .budget = 4 }) == TA_OK);
    CHECK(f.count == 0 && f.status == TA_RUN_PAUSED && in_state(&f, c, "c4"));
    CHECK(run_with(&f, (run_limits){ .budget = 4 }) == TA_OK);
    CHECK(f.count == 0 && f.status == TA_RUN_PAUSED && in_state(&f, c, "c8"));
    CHECK(in_state(&f, s, "s0")); /* the next input waits for the paused one */
    /* One microstep ends the chain; finding it stable takes none, so the
     * next input is taken with the budget left, and pauses in its turn. */
    CHECK(run_with(&f, (run_limits){ .budget = 1 }) == TA_OK);
    CHECK(f.count == 0 && f.status == TA_RUN_PAUSED && in_state(&f, c, "idle"));
    CHECK(run_with(&f, (run_limits){ .budget = 1 }) == TA_OK);
    CHECK(f.count == 1 && outcome_is(&f, 0, TA_INPUT_COMPLETED, c, 1) && f.status == TA_RUN_PAUSED);
    CHECK(in_state(&f, s, "s1"));
    CHECK(run_with(&f, (run_limits){ .budget = 1 }) == TA_OK);
    CHECK(f.count == 1 && outcome_is(&f, 0, TA_INPUT_COMPLETED, s, 2) && f.status == TA_RUN_IDLE);
    /* c1's entry has two units: an action budget of two is spent by the
     * chain exactly, and the next input waits. */
    CHECK(deliver(&f, c, "long", 3) == TA_OK);
    CHECK(deliver(&f, s, "e2", 4) == TA_OK);
    CHECK(run_with(&f, (run_limits){ .action_budget = 2 }) == TA_OK);
    CHECK(f.count == 1 && outcome_is(&f, 0, TA_INPUT_COMPLETED, c, 3) && f.status == TA_RUN_MORE);
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_COMPLETED, s, 4));
    /* An action budget of one pauses inside c1's entry, between its two logs. */
    CHECK(deliver(&f, c, "long", 5) == TA_OK);
    CHECK(run_with(&f, (run_limits){ .action_budget = 1 }) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, c, &view) == TA_OK);
    CHECK(f.status == TA_RUN_PAUSED && view.microstep_in_progress);
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_COMPLETED, c, 5));
    close_fixture(&f);
}

/* Claim 6. */
static void test_fault_stops_the_session(void) {
    ta_param_value limit[] = { { TA_PARAM_MICROSTEP_LIMIT, 4 } };
    fixture f = open_with(limit, 1);
    ta_actor_id c = spawn(&f, f.chain);
    ta_actor_id s = spawn(&f, f.sequence);
    CHECK(run_all(&f) == TA_OK && f.count == 2);
    CHECK(ta_actor_release(f.system, c) == TA_ERR_INVALID);
    CHECK(ta_actor_release(f.system, c + 1000u) == TA_ERR_NOT_FOUND);
    CHECK(deliver(&f, c, "long", 1) == TA_OK);
    CHECK(deliver(&f, s, "e1", 2) == TA_OK);
    CHECK(deliver(&f, c, "e1", 3) == TA_OK);  /* waits for c */
    CHECK(deliver(&f, s, "e2", 4) == TA_OK);
    CHECK(deliver(&f, c, "noise", 5) == TA_OK); /* and so does this, after it */
    CHECK(run_all(&f) == TA_OK);
    CHECK(f.count == 3 && f.status == TA_RUN_IDLE);
    CHECK(outcome_is(&f, 0, TA_INPUT_FAULTED, c, 1) && f.outcomes[0].code == TA_ERR_RUNAWAY);
    CHECK(outcome_is(&f, 1, TA_INPUT_COMPLETED, s, 2) && outcome_is(&f, 2, TA_INPUT_COMPLETED, s, 4));
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, c, &view) == TA_OK && view.macrostep_in_progress);
    CHECK(in_state(&f, c, "c4"));
    CHECK(run_all(&f) == TA_OK && f.count == 0 && f.status == TA_RUN_IDLE);
    /* Released, it ends without the limit; its deferred inputs follow in order. */
    CHECK(ta_actor_release(f.system, c) == TA_OK);
    CHECK(ta_actor_release(f.system, c) == TA_ERR_INVALID);
    CHECK(run_all(&f) == TA_OK);
    CHECK(f.count == 3 && f.status == TA_RUN_IDLE);
    CHECK(outcome_is(&f, 0, TA_INPUT_COMPLETED, c, 1));
    CHECK(outcome_is(&f, 1, TA_INPUT_COMPLETED, c, 3) && outcome_is(&f, 2, TA_INPUT_IGNORED, c, 5));
    CHECK(in_state(&f, c, "s1"));
    close_fixture(&f);

    /* The next macrostep has the limit again. */
    f = open_with(limit, 1);
    c = spawn(&f, f.chain);
    CHECK(run_all(&f) == TA_OK);
    CHECK(deliver(&f, c, "long", 1) == TA_OK);
    CHECK(run_all(&f) == TA_OK && outcome_is(&f, 0, TA_INPUT_FAULTED, c, 1));
    CHECK(ta_actor_release(f.system, c) == TA_OK);
    CHECK(run_all(&f) == TA_OK && outcome_is(&f, 0, TA_INPUT_COMPLETED, c, 1));
    CHECK(deliver(&f, c, "long", 2) == TA_OK);
    CHECK(run_all(&f) == TA_OK && outcome_is(&f, 0, TA_INPUT_FAULTED, c, 2) &&
          f.outcomes[0].code == TA_ERR_RUNAWAY);
    close_fixture(&f); /* a stopped input goes with its session */
}

/* Claim 7. */
static void test_refusals(void) {
    fixture f = open_default();
    ta_actor_id gone = spawn(&f, f.sequence);
    ta_actor_id s = spawn(&f, f.sequence);
    CHECK(run_all(&f) == TA_OK && f.count == 2);
    CHECK(deliver(&f, gone, "e1", 1) == TA_OK);
    CHECK(ta_actor_destroy(f.system, gone) == TA_OK);
    CHECK(deliver(&f, s, "stop", 2) == TA_OK);
    CHECK(deliver(&f, s, "e1", 3) == TA_OK);
    ta_actor_id never = spawn(&f, f.sequence); /* destroyed before its bootstrap: dropped unreported */
    CHECK(ta_actor_destroy(f.system, never) == TA_OK);
    CHECK(run_all(&f) == TA_OK && f.count == 3);
    CHECK(outcome_is(&f, 0, TA_INPUT_REFUSED, gone, 1) && f.outcomes[0].code == TA_ERR_NOT_FOUND);
    CHECK(outcome_is(&f, 1, TA_INPUT_FINISHED, s, 2));
    CHECK(outcome_is(&f, 2, TA_INPUT_REFUSED, s, 3) && f.outcomes[2].code == TA_ERR_DONE);
    CHECK(deliver(&f, s, "e1", 4) == TA_ERR_DONE);

    /* A stopped session's deferred inputs are refused with it when it goes. */
    close_fixture(&f);
    ta_param_value limit[] = { { TA_PARAM_MICROSTEP_LIMIT, 4 } };
    f = open_with(limit, 1);
    ta_actor_id c = spawn(&f, f.chain);
    CHECK(run_all(&f) == TA_OK);
    CHECK(deliver(&f, c, "long", 1) == TA_OK);
    CHECK(deliver(&f, c, "e1", 2) == TA_OK);
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_FAULTED, c, 1));
    CHECK(ta_actor_destroy(f.system, c) == TA_OK);
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_REFUSED, c, 2));

    /* An operator transition whose source is not active when it is taken. */
    ta_actor_id j = spawn(&f, f.sequence);
    CHECK(run_all(&f) == TA_OK);
    ta_state_id s0 = 0;
    ta_state_id s2 = 0;
    CHECK(ta_definition_find_state(f.system, &(ta_definition_find_state_params){ .definition = f.sequence,
                                                                                .id = STR("s0"), .out = &s0 }) ==
          TA_OK);
    CHECK(ta_definition_find_state(f.system, &(ta_definition_find_state_params){ .definition = f.sequence,
                                                                                .id = STR("s2"), .out = &s2 }) ==
          TA_OK);
    CHECK(deliver(&f, j, "e1", 3) == TA_OK);
    ta_event jump = { .name = name_of(&f, "operator.goto"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_operator_transition from_s0 = { .source = s0, .targets = &s2, .target_count = 1 };
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .actor = j, .name = test_event_text(f.system, &jump), .event = &jump,
                                                                 .time = ta_system_time(f.system),
                                                                 .transition = &from_s0, .correlation = 4 }) ==
          TA_OK); /* s0 is active now; e1 leaves it first */
    CHECK(deliver(&f, j, "e2", 5) == TA_OK);
    long live = heap.live;
    CHECK(run_all(&f) == TA_OK && f.count == 3);
    CHECK(outcome_is(&f, 1, TA_INPUT_REFUSED, j, 4) && f.outcomes[1].code == TA_ERR_INVALID);
    CHECK(outcome_is(&f, 2, TA_INPUT_COMPLETED, j, 5) && in_state(&f, j, "s2"));
    CHECK(heap.live < live); /* the operator transition went with it */
    close_fixture(&f);
}

/* Claim 8. */
static void test_withdraw(void) {
    ta_param_value limit[] = { { TA_PARAM_MICROSTEP_LIMIT, 4 } };
    fixture f = open_with(limit, 1);
    ta_actor_id s = spawn(&f, f.sequence);
    ta_actor_id c = spawn(&f, f.chain);
    CHECK(run_all(&f) == TA_OK);
    counted value = { .references = 1 };
    ta_event event = { .name = name_of(&f, "e1"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE,
                       .data = { .self = &value, .v = &counted_vtable } };
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .actor = s, .name = test_event_text(f.system, &event), .event = &event,
                                                                 .time = ta_system_time(f.system),
                                                                 .correlation = 1 }) == TA_OK);
    CHECK(value.references == 2);
    CHECK(ta_system_withdraw_input(f.system, 0) == TA_ERR_INVALID);
    CHECK(ta_system_withdraw_input(f.system, 9) == TA_ERR_NOT_FOUND);
    CHECK(ta_system_withdraw_input(f.system, 1) == TA_OK);
    CHECK(value.references == 1);
    CHECK(ta_system_withdraw_input(f.system, 1) == TA_ERR_NOT_FOUND);
    CHECK(run_all(&f) == TA_OK && f.count == 0 && in_state(&f, s, "s0"));
    /* Deferred for a stopped session, it is withdrawn from its deferred list. */
    CHECK(deliver(&f, c, "long", 2) == TA_OK);
    CHECK(deliver(&f, c, "e1", 3) == TA_OK);
    CHECK(deliver(&f, c, "noise", 4) == TA_OK);
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_FAULTED, c, 2));
    CHECK(ta_list_head(&ta_actor_lookup(f.system, c)->deferred) != 0u);
    CHECK(ta_system_withdraw_input(f.system, 3) == TA_OK);
    CHECK(ta_system_withdraw_input(f.system, 2) == TA_ERR_NOT_FOUND); /* taken */
    CHECK(ta_actor_release(f.system, c) == TA_OK);
    CHECK(ta_system_withdraw_input(f.system, 2) == TA_ERR_NOT_FOUND); /* released, still taken */
    CHECK(run_all(&f) == TA_OK && f.count == 2);
    CHECK(outcome_is(&f, 0, TA_INPUT_COMPLETED, c, 2) && outcome_is(&f, 1, TA_INPUT_IGNORED, c, 4));
    CHECK(in_state(&f, c, "idle"));
    close_fixture(&f);
}

/* Claim 9. */
static void test_session_filter(void) {
    fixture f = open_default();
    ta_actor_id a = spawn(&f, f.sequence);
    ta_actor_id c = spawn(&f, f.chain);
    CHECK(run_with(&f, (run_limits){ .session = c }) == TA_OK);
    CHECK(f.count == 1 && outcome_is(&f, 0, TA_INPUT_COMPLETED, c, 0) && f.status == TA_RUN_IDLE);
    CHECK(run_with(&f, (run_limits){ .session = a }) == TA_OK);
    CHECK(f.count == 1 && outcome_is(&f, 0, TA_INPUT_COMPLETED, a, 0));
    CHECK(deliver(&f, a, "e1", 1) == TA_OK);
    CHECK(deliver(&f, c, "long", 2) == TA_OK);
    CHECK(deliver(&f, a, "e2", 3) == TA_OK);
    /* c's input, behind a's, runs first, and pauses in the slot. */
    CHECK(run_with(&f, (run_limits){ .session = c, .budget = 2 }) == TA_OK);
    CHECK(f.count == 0 && f.status == TA_RUN_PAUSED && in_state(&f, a, "s0"));
    /* a's inputs run, the slot left as it stands. */
    CHECK(run_with(&f, (run_limits){ .session = a }) == TA_OK);
    CHECK(f.count == 2 && f.status == TA_RUN_IDLE && in_state(&f, a, "s2"));
    CHECK(outcome_is(&f, 0, TA_INPUT_COMPLETED, a, 1) && outcome_is(&f, 1, TA_INPUT_COMPLETED, a, 3));
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, c, &view) == TA_OK && view.macrostep_in_progress);
    /* a second paused session, beside the slot */
    CHECK(deliver(&f, a, "e3", 4) == TA_OK);
    CHECK(run_with(&f, (run_limits){ .session = a, .budget = 1 }) == TA_OK);
    CHECK(f.status == TA_RUN_PAUSED);
    /* Unfiltered, the slot goes on first, then what waits. */
    CHECK(run_all(&f) == TA_OK && f.count == 2 && f.status == TA_RUN_IDLE);
    CHECK(outcome_is(&f, 0, TA_INPUT_COMPLETED, c, 2) && outcome_is(&f, 1, TA_INPUT_COMPLETED, a, 4));
    close_fixture(&f);
}

/* An invoker whose service starts and never reports by itself. */
static ta_op_status svc_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
    *params->service_handle = 1;
    return TA_OP_OK;
}

static ta_op_status svc_cancel(void *self, uint64_t service_handle, ta_fault *fault) {
    (void)self;
    (void)service_handle;
    (void)fault;
    return TA_OP_OK;
}

static ta_op_status svc_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self;
    (void)params;
    return TA_OP_OK;
}

static const ta_invoker_vtable svc_vtable = { .start = svc_start, .cancel = svc_cancel, .forward = svc_forward };

static ta_result report(fixture *f, ta_actor_id actor, ta_invoke_id invoke, uint64_t correlation) {
    return ta_actor_report_invoke(f->system, &(ta_invoke_report_params){
                                                 .actor = actor,
                                                 .invoke_id = invoke,
                                                 .kind = TA_INVOKE_REPORT_ERROR,
                                                 .time = ta_system_time(f->system),
                                                 .correlation = correlation,
                                                 .as.error = &(ta_invoke_error){ .reason = STR("lost"), .ends = true },
                                             });
}

/* Claim 10. */
static void test_error_report(void) {
    fixture f = open_default();
    ta_atom type = 0;
    CHECK(ta_system_intern_atom(f.system, STR("svc"), &type) == TA_OK);
    CHECK(ta_system_register_invoker(f.system, type, (ta_invoker){ .v = &svc_vtable }) == TA_OK);
    ta_definition_id id = define(f.system, &invoking);
    ta_actor_id actor = spawn(&f, id);
    CHECK(run_all(&f) == TA_OK && f.count == 1);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, actor, &view) == TA_OK && view.invoke_count == 1);
    ta_invoke_id invoke = view.invokes[0].id;
    CHECK(report(&f, actor, invoke, 1) == TA_OK);
    CHECK(report(&f, actor, invoke, 2) == TA_OK); /* the invocation is still active */
    CHECK(ta_actor_inspect(f.system, actor, &view) == TA_OK && view.invoke_count == 1 && !view.macrostep_in_progress);
    CHECK(run_all(&f) == TA_OK && f.count == 2);
    CHECK(outcome_is(&f, 0, TA_INPUT_COMPLETED, actor, 1) && in_state(&f, actor, "failed"));
    /* The first ended the invocation: the second is refused. */
    CHECK(outcome_is(&f, 1, TA_INPUT_REFUSED, actor, 2) && f.outcomes[1].code == TA_ERR_NOT_FOUND);
    CHECK(report(&f, actor, invoke, 3) == TA_ERR_NOT_FOUND);
    close_fixture(&f);

    /* At the watermark a report bounces, as an event does. */
    ta_param_value small_pool[] = { { TA_PARAM_POOL_ENVELOPES, 8 }, { TA_PARAM_ADMISSION_WATERMARK, 25 } };
    f = open_with(small_pool, 2);
    CHECK(ta_system_intern_atom(f.system, STR("svc"), &type) == TA_OK);
    CHECK(ta_system_register_invoker(f.system, type, (ta_invoker){ .v = &svc_vtable }) == TA_OK);
    id = define(f.system, &invoking);
    actor = spawn(&f, id);
    CHECK(run_all(&f) == TA_OK);
    CHECK(ta_actor_inspect(f.system, actor, &view) == TA_OK && view.invoke_count == 1);
    invoke = view.invokes[0].id;
    CHECK(report(&f, actor, invoke, 1) == TA_OK);
    CHECK(report(&f, actor, invoke, 2) == TA_OK);
    CHECK(report(&f, actor, invoke, 3) == TA_ERR_FULL);
    CHECK(run_all(&f) == TA_OK && f.count == 2);
    close_fixture(&f);
}

/* A log sink that tries to run the system from inside a run. */
static ta_system *reentered = NULL;
static ta_result reentry = TA_OK;

static void reenter(void *self, const ta_log_record *record) {
    (void)self;
    (void)record;
    ta_input_outcome outcome;
    uint32_t count = 7;
    ta_run_status status = TA_RUN_IDLE;
    reentry = ta_run(reentered, &(ta_run_params){ .budget = 10, .outcomes = &outcome, .outcome_capacity = 1,
                                                  .outcome_count = &count, .status = &status });
    CHECK(count == 0);
}

static const ta_log_sink_vtable reenter_vtable = { .write = reenter };

/* Claim 11. */
static void test_bad_calls(void) {
    fixture f = open_default();
    ta_input_outcome outcome;
    uint32_t count = 0;
    ta_run_status status = TA_RUN_IDLE;
    CHECK(ta_run(NULL, &(ta_run_params){ .budget = 1, .outcome_count = &count, .status = &status }) == TA_ERR_INVALID);
    CHECK(ta_run(f.system, NULL) == TA_ERR_INVALID);
    CHECK(ta_run(f.system, &(ta_run_params){ .budget = 0, .outcome_count = &count, .status = &status }) ==
          TA_ERR_INVALID);
    CHECK(ta_run(f.system, &(ta_run_params){ .budget = 1, .status = &status }) == TA_ERR_INVALID);
    CHECK(ta_run(f.system, &(ta_run_params){ .budget = 1, .outcome_count = &count }) == TA_ERR_INVALID);
    CHECK(ta_run(f.system, &(ta_run_params){ .budget = 1, .outcome_capacity = 1, .outcome_count = &count,
                                             .status = &status }) == TA_ERR_INVALID);
    CHECK(ta_actor_release(NULL, 1) == TA_ERR_INVALID);
    CHECK(ta_system_withdraw_input(NULL, 1) == TA_ERR_INVALID);
    CHECK(ta_actor_deliver(f.system, NULL) == TA_ERR_INVALID);

    ta_actor_id c = spawn(&f, f.chain);
    CHECK(run_all(&f) == TA_OK);
    reentered = f.system;
    reentry = TA_OK;
    CHECK(ta_system_set_log_sink(f.system, (ta_log_sink){ .v = &reenter_vtable }) == TA_OK);
    CHECK(deliver(&f, c, "long", 1) == TA_OK);
    CHECK(ta_run(f.system, &(ta_run_params){ .budget = 100, .outcomes = &outcome, .outcome_capacity = 1,
                                             .outcome_count = &count, .status = &status }) == TA_OK);
    CHECK(reentry == TA_ERR_BUSY);
    CHECK(count == 1 && outcome.kind == TA_INPUT_COMPLETED && outcome.correlation == 1);
    close_fixture(&f);
}

/* Claim 12. */
static void test_out_of_memory(void) {
    fixture f = open_default();
    ta_actor_id s = spawn(&f, f.sequence);
    CHECK(run_all(&f) == TA_OK);
    /* No allocation at all: admission takes what is free. */
    heap.policy.refuse = true;
    CHECK(deliver(&f, s, "e1", 1) == TA_OK);
    CHECK(deliver(&f, s, "e2", 2) == TA_OK);
    /* Once nothing is free, the refill's failure is the reason: TA_ERR_NOMEM. */
    uint64_t extra = 3u;
    ta_result admitted = TA_OK;
    while (extra < 1000u && (admitted = deliver(&f, s, "e1", extra)) == TA_OK) {
        extra += 1u;
    }
    CHECK(admitted == TA_ERR_NOMEM);
    for (uint64_t correlation = 3u; correlation < extra; correlation += 1u) {
        CHECK(ta_system_withdraw_input(f.system, correlation) == TA_OK);
    }
    /* Taking an event allocates nothing: its event stays in its envelope. */
    CHECK(run_all(&f) == TA_OK && f.count == 2);
    CHECK(outcome_is(&f, 0, TA_INPUT_COMPLETED, s, 1) && outcome_is(&f, 1, TA_INPUT_COMPLETED, s, 2));
    CHECK(in_state(&f, s, "s2"));
    heap.policy.refuse = false;
    close_fixture(&f);

    /* A report's event needs room in its session's internal queue. While
     * another session's paused macrostep holds the system's spare queue,
     * that room is allocated: without it the run takes nothing, and the
     * report waits at the head. */
    f = open_default();
    ta_atom type = 0;
    CHECK(ta_system_intern_atom(f.system, STR("svc"), &type) == TA_OK);
    CHECK(ta_system_register_invoker(f.system, type, (ta_invoker){ .v = &svc_vtable }) == TA_OK);
    ta_actor_id actor = spawn(&f, define(f.system, &invoking));
    ta_actor_id c = spawn(&f, f.chain);
    CHECK(run_all(&f) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, actor, &view) == TA_OK && view.invoke_count == 1);
    CHECK(deliver(&f, c, "long", 1) == TA_OK);
    CHECK(run_with(&f, (run_limits){ .session = c, .budget = 2 }) == TA_OK && f.status == TA_RUN_PAUSED);
    CHECK(report(&f, actor, view.invokes[0].id, 2) == TA_OK);
    heap.policy.refuse = true;
    CHECK(run_with(&f, (run_limits){ .session = actor }) == TA_ERR_NOMEM);
    CHECK(f.count == 0 && f.status == TA_RUN_MORE && !in_state(&f, actor, "failed"));
    heap.policy.refuse = false;
    CHECK(run_with(&f, (run_limits){ .session = actor }) == TA_OK && f.count == 1);
    CHECK(outcome_is(&f, 0, TA_INPUT_COMPLETED, actor, 2) && in_state(&f, actor, "failed"));
    CHECK(run_all(&f) == TA_OK && f.count == 1 && outcome_is(&f, 0, TA_INPUT_COMPLETED, c, 1));
    close_fixture(&f);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_admission_runs_nothing();
    test_full_pool_bounces();
    test_order_per_sender_and_receiver();
    test_outcomes_once();
    test_budgets();
    test_fault_stops_the_session();
    test_refusals();
    test_withdraw();
    test_session_filter();
    test_error_report();
    test_bad_calls();
    test_out_of_memory();
    if (failures != 0) {
        fprintf(stderr, "run_test: %d checks failed\n", failures);
        return 1;
    }
    printf("run_test: ok\n");
    return 0;
}
