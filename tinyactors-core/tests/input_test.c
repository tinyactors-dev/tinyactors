/* test: standalone asan */
/*
 * Host inputs (src/run.c; ADR 0099 sections 5 and 8; ADR 0099's notes for
 * unit 4.2e), through the amalgamation because the tests read the pool and a
 * session's `queued` count:
 *
 *   1. Withdrawing: a queued input never runs and writes no notice; its
 *      session's `queued` and ta_counters.host_inputs go down at once; a
 *      token that no longer names a pending host input (taken, completed,
 *      refused, withdrawn, stale after the envelope was reused) answers
 *      TA_ERR_NOT_FOUND and touches nothing; an input waiting for an
 *      address can be withdrawn, and the list's load cue is still written
 *      once.
 *   2. The gauge: ta_counters.host_inputs is the number of host inputs
 *      admitted and not decided, after every way an input can end.
 *
 * The exhaustive sweep (input_sweep_test.c) covers the allocation-failure
 * paths of these calls.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_allocator.h"
#include "run_support.h"

static int failures;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                                 \
        }                                                                                  \
    } while (0)

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }
#define END {0}

static ta_test_allocator heap = { .strict = true };
static const ta_allocator allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

#define ON(event_text, target_text)                                                    \
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR(event_text),                  \
      .targets = (const ta_str[]){ STR(target_text), END } }

/* ------------------------------------------------------------------------
 * The world: `chain` goes s0 -> s1 -> s2 -> s3 on e1, e2, e3 (only the order
 * e1 e2 e3 reaches s3, so the state tells which inputs ran and in what order);
 * `finisher` is final after `end`.
 */

typedef struct {
    ta_system *system;
    ta_definition_id chain, finisher;
    ta_event_name_id e1, e2, e3, end;
} world;

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &atom) == TA_OK);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &name) == TA_OK);
    return name;
}

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

static void world_init(world *w, const ta_param_value *params, uint32_t count) {
    *w = (world){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params, .param_count = count },
                           &w->system) == TA_OK);
    ta_scxml_state_spec chain[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s0"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("e1", "s1"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s1"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("e2", "s2"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s2"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("e3", "s3"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s3") },
        END,
    };
    ta_scxml_state_spec finisher[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("wait"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("end", "over"), END } },
        { .kind = TA_SCXML_STATE_FINAL, .id = STR("over") },
        END,
    };
    w->chain = define(w->system, &(ta_scxml_spec){ .name = STR("chain"), .states = chain });
    w->finisher = define(w->system, &(ta_scxml_spec){ .name = STR("finisher"), .states = finisher });
    w->e1 = event_name(w->system, "e1");
    w->e2 = event_name(w->system, "e2");
    w->e3 = event_name(w->system, "e3");
    w->end = event_name(w->system, "end");
}

static void world_close(world *w) {
    ta_system_destroy(w->system);
    CHECK(heap.stats.live_blocks == 0);
}

static ta_actor_id spawn(world *w, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(w->system, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    return actor;
}

static ta_result post(world *w, ta_actor_id actor, ta_event_name_id name, uint64_t *token) {
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(w->system, &(ta_actor_deliver_params){ .actor = actor, .event = &event,
                                                                   .time = ta_system_time(w->system),
                                                                   .input = token });
}

static ta_result post_to(world *w, ta_address *address, ta_event_name_id name, uint64_t *token) {
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(w->system, &(ta_actor_deliver_params){ .address = address, .event = &event,
                                                                   .time = ta_system_time(w->system),
                                                                   .input = token });
}

static ta_address *intern_address(world *w, const char *name) {
    ta_address *address = NULL;
    CHECK(ta_address_intern(w->system, &(ta_actor_address){ .family = { .name = STR("chain") },
                                                            .name = { .ptr = name, .len = (uint32_t)strlen(name) } },
                            &address) == TA_OK);
    return address;
}

typedef struct {
    ta_run_status status;
    ta_run_notice notices[32];
    uint32_t count;
} report;

/* One ta_run call with a large budget; the notices are valid until the next call. */
static ta_result run(world *w, bool taken, report *out) {
    out->count = 0;
    return ta_run(w->system, &(ta_run_params){ .budget = 1000, .report_taken = taken, .status = &out->status,
                                               .notices = out->notices, .notice_capacity = 32,
                                               .notice_count = &out->count });
}

static uint32_t count_kind(const report *r, ta_run_notice_kind kind) {
    uint32_t n = 0;
    for (uint32_t index = 0; index < r->count; index += 1u) {
        n += r->notices[index].kind == kind ? 1u : 0u;
    }
    return n;
}

/* Counts the notices of `kind` that carry `token`. */
static uint32_t count_token(const report *r, ta_run_notice_kind kind, uint64_t token) {
    uint32_t n = 0;
    for (uint32_t index = 0; index < r->count; index += 1u) {
        n += r->notices[index].kind == kind && r->notices[index].input == token ? 1u : 0u;
    }
    return n;
}

static uint64_t gauge(world *w) {
    ta_counters counters;
    ta_system_counters(w->system, &counters);
    return counters.host_inputs;
}

static uint32_t queued_of(world *w, ta_actor_id id) {
    const ta_actor *actor = ta_actor_lookup(w->system, id);
    return actor == NULL ? 0xffffffffu : actor->queued;
}

static bool in_state(world *w, ta_actor_id actor, const char *id) {
    ta_state_id active[8];
    uint32_t count = 0;
    ta_actor_inspection inspection = {0};
    CHECK(ta_actor_inspect(w->system, actor, &inspection) == TA_OK);
    CHECK(ta_actor_active_states(w->system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                             .capacity = 8, .count = &count }) ==
          TA_OK);
    for (uint32_t index = 0; index < count; index += 1) {
        ta_state_info info = {0};
        CHECK(ta_definition_state(w->system, &(ta_definition_state_params){ .definition = inspection.definition_id,
                                                                            .state = active[index], .out = &info }) ==
              TA_OK);
        if (info.id.len == strlen(id) && memcmp(info.id.ptr, id, info.id.len) == 0) {
            return true;
        }
    }
    return false;
}

/* Runs the system to IDLE, then once more (that call frees what the last one lent). */
static void settle(world *w) {
    report r;
    do {
        CHECK(run(w, false, &r) == TA_OK);
    } while (r.status != TA_RUN_IDLE);
    CHECK(run(w, false, &r) == TA_OK);
}

/* The pool is back to nothing in use. */
static void quiescent(world *w) {
    const ta_envelopes *envelopes = &w->system->envelopes;
    CHECK(envelopes->pool.used == 0);
    CHECK(envelopes->quota.used == 0 && envelopes->quota.reserved == 0);
    CHECK(ta_ilist_is_empty(&envelopes->run));
    CHECK(gauge(w) == 0);
}

/* ------------------------------------------------------------------------
 * 1. Withdrawing
 */

/* A withdrawn input never runs, writes no notice and lets the others keep their order. */
static void test_withdraw_a_queued_input(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.chain);
    settle(&w);
    uint64_t first = 0, second = 0, third = 0;
    CHECK(post(&w, actor, w.e1, &first) == TA_OK);
    CHECK(post(&w, actor, w.e2, &second) == TA_OK);
    CHECK(post(&w, actor, w.e3, &third) == TA_OK);
    CHECK(gauge(&w) == 3u && queued_of(&w, actor) == 3u);

    CHECK(ta_input_withdraw(w.system, second) == TA_OK);
    CHECK(gauge(&w) == 2u && queued_of(&w, actor) == 2u);
    CHECK(ta_input_withdraw(w.system, second) == TA_ERR_NOT_FOUND); /* once */
    CHECK(gauge(&w) == 2u);

    report r;
    CHECK(run(&w, true, &r) == TA_OK && r.status == TA_RUN_IDLE);
    CHECK(count_token(&r, TA_NOTICE_INPUT_TAKEN, first) == 1u && count_token(&r, TA_NOTICE_INPUT_TAKEN, third) == 1u);
    CHECK(count_token(&r, TA_NOTICE_INPUT_TAKEN, second) == 0u);
    CHECK(count_token(&r, TA_NOTICE_INPUT_COMPLETED, second) == 0u);
    CHECK(count_kind(&r, TA_NOTICE_INPUT_REFUSED) == 0u);
    CHECK(in_state(&w, actor, "s1")); /* e1 ran; e3 found s1 and enabled nothing: e2 never ran */
    CHECK(gauge(&w) == 0u && queued_of(&w, actor) == 0u);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* A token that names no pending input is NOT_FOUND and changes nothing. */
static void test_withdraw_answers_not_found(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.chain);
    settle(&w);
    CHECK(ta_input_withdraw(NULL, 1u) == TA_ERR_INVALID);
    CHECK(ta_input_withdraw(w.system, 0) == TA_ERR_NOT_FOUND);
    CHECK(ta_input_withdraw(w.system, UINT64_MAX) == TA_ERR_NOT_FOUND);
    CHECK(ta_input_withdraw(w.system, 12345u) == TA_ERR_NOT_FOUND);

    uint64_t token = 0;
    CHECK(post(&w, actor, w.e1, &token) == TA_OK);
    CHECK(ta_input_withdraw(w.system, token ^ (UINT64_C(1) << 40)) == TA_ERR_NOT_FOUND); /* the sequence is part of it */
    CHECK(gauge(&w) == 1u);
    report r;
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(count_token(&r, TA_NOTICE_INPUT_COMPLETED, token) == 1u);
    CHECK(ta_input_withdraw(w.system, token) == TA_ERR_NOT_FOUND); /* completed */
    CHECK(gauge(&w) == 0u);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* A token whose envelope was released and used again does not reach the new input. */
static void test_withdraw_with_a_stale_token(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.chain);
    settle(&w);
    uint64_t old = 0, fresh = 0;
    CHECK(post(&w, actor, w.e1, &old) == TA_OK);
    CHECK(ta_input_withdraw(w.system, old) == TA_OK);
    settle(&w); /* the withdrawn envelope is released unseen */
    CHECK(w.system->envelopes.pool.used == 0);
    CHECK(post(&w, actor, w.e1, &fresh) == TA_OK);
    CHECK(fresh != old);
    CHECK((fresh & TA_INPUT_INDEX_MASK) == (old & TA_INPUT_INDEX_MASK)); /* the same envelope, reused */
    CHECK(ta_input_withdraw(w.system, old) == TA_ERR_NOT_FOUND);
    CHECK(gauge(&w) == 1u && queued_of(&w, actor) == 1u);
    CHECK(ta_input_withdraw(w.system, fresh) == TA_OK);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* A taken input cannot be withdrawn: the macrostep that holds it runs on. */
static void test_withdraw_a_taken_input(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.chain);
    settle(&w);
    uint64_t token = 0;
    CHECK(post(&w, actor, w.e1, &token) == TA_OK);
    ta_run_status status = TA_RUN_IDLE;
    ta_run_notice notices[8];
    uint32_t count = 0;
    CHECK(ta_run(w.system, &(ta_run_params){ .budget = 1, .status = &status, .notices = notices, .notice_capacity = 8,
                                             .notice_count = &count }) == TA_OK);
    CHECK(status == TA_RUN_PAUSED && w.system->runner.slot.occupied);
    CHECK(ta_input_withdraw(w.system, token) == TA_ERR_NOT_FOUND);
    CHECK(gauge(&w) == 1u); /* still its macrostep's */
    settle(&w);
    CHECK(in_state(&w, actor, "s1"));
    quiescent(&w);
    world_close(&w);
}

/* An input for a session that went away is decided by its REFUSED notice, once. */
static void test_withdraw_a_refused_input(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id doomed = spawn(&w, w.chain);
    settle(&w);
    uint64_t token = 0;
    CHECK(post(&w, doomed, w.e1, &token) == TA_OK);
    CHECK(ta_actor_destroy(w.system, doomed) == TA_OK);
    CHECK(gauge(&w) == 1u); /* admitted, not yet decided: the run decides it */
    report r;
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(count_token(&r, TA_NOTICE_INPUT_REFUSED, token) == 1u);
    CHECK(gauge(&w) == 0u);
    CHECK(ta_input_withdraw(w.system, token) == TA_ERR_NOT_FOUND); /* its notice lends the event now */
    CHECK(gauge(&w) == 0u);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/*
 * An input that waits for an address can be withdrawn. The list keeps its
 * place and its cue: one LOAD_NEEDED is still written, and a failed load
 * refuses only the inputs that were not withdrawn.
 */
static void test_withdraw_a_waiting_input(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_address *address = intern_address(&w, "n1");
    uint64_t first = 0, second = 0;
    CHECK(post_to(&w, address, w.e1, &first) == TA_OK);
    CHECK(post_to(&w, address, w.e2, &second) == TA_OK);
    CHECK(ta_address_waiting(w.system, address) == 2u && gauge(&w) == 2u);
    CHECK(ta_input_withdraw(w.system, first) == TA_OK); /* the first one holds the cue */
    CHECK(ta_address_waiting(w.system, address) == 1u && gauge(&w) == 1u);

    report r;
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(count_kind(&r, TA_NOTICE_LOAD_NEEDED) == 1u);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = address,
                                                                            .reason = TA_REFUSED_LOAD_NOT_FOUND }) ==
          TA_OK);
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(count_token(&r, TA_NOTICE_INPUT_REFUSED, second) == 1u);
    CHECK(count_token(&r, TA_NOTICE_INPUT_REFUSED, first) == 0u);
    CHECK(gauge(&w) == 0u);
    settle(&w);
    quiescent(&w);
    ta_address_release(address);
    world_close(&w);
}

/* When every input of a list was withdrawn, the list still exists until the host answers its cue. */
static void test_withdraw_a_whole_waiting_list(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_address *address = intern_address(&w, "n1");
    uint64_t token = 0;
    CHECK(post_to(&w, address, w.e1, &token) == TA_OK);
    CHECK(ta_input_withdraw(w.system, token) == TA_OK);
    CHECK(ta_address_waiting(w.system, address) == 0u);
    CHECK(ta_waiting_length(&w.system->envelopes, address) == 1u); /* it holds the list's place */
    report r;
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(count_kind(&r, TA_NOTICE_LOAD_NEEDED) == 1u);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = address,
                                                                            .reason = TA_REFUSED_LOAD_FAILED }) ==
          TA_OK);
    CHECK(ta_waiting_length(&w.system->envelopes, address) == 0u);
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(count_kind(&r, TA_NOTICE_INPUT_REFUSED) == 0u);
    settle(&w);
    quiescent(&w);
    ta_address_release(address);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 2. The gauge
 */

/* host_inputs follows an input through every way it can end. */
static void test_gauge_follows_every_end(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id chain = spawn(&w, w.chain);
    ta_actor_id finisher = spawn(&w, w.finisher);
    settle(&w);
    CHECK(gauge(&w) == 0u);

    /* completed */
    uint64_t token = 0;
    CHECK(post(&w, chain, w.e1, &token) == TA_OK && gauge(&w) == 1u);
    settle(&w);
    CHECK(gauge(&w) == 0u);

    /* done, then refused: the first ends the session, the second finds it finished */
    uint64_t finishing = 0, late = 0;
    CHECK(post(&w, finisher, w.end, &finishing) == TA_OK);
    CHECK(post(&w, finisher, w.end, &late) == TA_OK && gauge(&w) == 2u);
    report r;
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(count_token(&r, TA_NOTICE_INPUT_COMPLETED, finishing) == 1u);
    CHECK(count_token(&r, TA_NOTICE_INPUT_REFUSED, late) == 1u);
    CHECK(gauge(&w) == 0u);

    /* abandoned: a paused macrostep whose session is destroyed */
    CHECK(post(&w, chain, w.e2, &token) == TA_OK && gauge(&w) == 1u);
    ta_run_status status = TA_RUN_IDLE;
    ta_run_notice notices[8];
    uint32_t count = 0;
    CHECK(ta_run(w.system, &(ta_run_params){ .budget = 1, .status = &status, .notices = notices, .notice_capacity = 8,
                                             .notice_count = &count }) == TA_OK);
    CHECK(status == TA_RUN_PAUSED && gauge(&w) == 1u);
    CHECK(ta_actor_destroy(w.system, chain) == TA_OK);
    CHECK(gauge(&w) == 0u); /* decided: the next run reports it */
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(count_token(&r, TA_NOTICE_INPUT_COMPLETED, token) == 1u && r.notices[0].status == TA_INPUT_ABANDONED);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

int main(void) {
    test_withdraw_a_queued_input();
    test_withdraw_answers_not_found();
    test_withdraw_with_a_stale_token();
    test_withdraw_a_taken_input();
    test_withdraw_a_refused_input();
    test_withdraw_a_waiting_input();
    test_withdraw_a_whole_waiting_list();
    test_gauge_follows_every_end();
    if (failures != 0) {
        fprintf(stderr, "input_test: %d checks failed\n", failures);
        return 1;
    }
    printf("input_test: ok\n");
    return 0;
}
