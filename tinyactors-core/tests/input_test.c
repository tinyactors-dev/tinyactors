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
 *   3. Admission inside a macrostep: a data-model script calls
 *      ta_actor_deliver and ta_input_withdraw while ta_run executes. The input
 *      is paid from the macrostep's reserve, waits behind the mail the
 *      macrostep sent before it and is not visible until the macrostep ends;
 *      a spent reserve answers TA_ERR_FULL; an abandoned or runaway
 *      macrostep drops it and ta_run reports REFUSED with
 *      TA_REFUSED_DISCARDED and its token.
 *   4. Invocation error reports: ta_actor_report_invoke with
 *      TA_INVOKE_REPORT_ERROR is an input of source ERROR; it never answers
 *      TA_ERR_BUSY, runs in the order of admission, and is applied (and the
 *      invocation retired when it ends it) only when ta_run takes it.
 *   6. The input watch offers every taken input, host inputs included.
 *   5. Inspection: ta_actor_pending lists a session's waiting inputs in run
 *      order without allocating; a TAKEN notice carries the input's time.
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
    for (uint32_t index = 0; index < r.count; index += 1u) {
        if (r.notices[index].kind == TA_NOTICE_INPUT_REFUSED) {
            CHECK(r.notices[index].address == address); /* the refusal names the address the input waited for */
            CHECK(r.notices[index].reason == TA_REFUSED_LOAD_NOT_FOUND);
        }
    }
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

/* A refused input that did not wait for an address names none. */
static void test_refusal_without_an_address(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id doomed = spawn(&w, w.chain);
    settle(&w);
    uint64_t token = 0;
    CHECK(post(&w, doomed, w.e1, &token) == TA_OK);
    CHECK(ta_actor_destroy(w.system, doomed) == TA_OK);
    report r;
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(count_token(&r, TA_NOTICE_INPUT_REFUSED, token) == 1u);
    for (uint32_t index = 0; index < r.count; index += 1u) {
        CHECK(r.notices[index].kind != TA_NOTICE_INPUT_REFUSED || r.notices[index].address == NULL);
    }
    settle(&w);
    quiescent(&w);
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

/* ------------------------------------------------------------------------
 * 3. Admission inside a macrostep
 */

/*
 * A data model whose scripts call back into the host while the macrostep
 * executes. The script's text says what the host does: "post:e1" delivers e1
 * to `hook.target`, "withdraw" withdraws the last token it got. Each call
 * records its answer in `hook`, and the state it saw right after.
 */
enum { HOOK_MAX = 8 };

typedef struct {
    world *w;
    ta_actor_id target;
    uint32_t calls;
    uint32_t admitted; /* how many posts answered TA_OK, however many calls were made */
    ta_result results[HOOK_MAX];
    uint64_t tokens[HOOK_MAX];
    bool executing[HOOK_MAX];       /* ta_run_executing() at the call */
    uint64_t attempts[HOOK_MAX];    /* allocator calls the delivery made */
    bool run_queue_empty[HOOK_MAX]; /* the run queue was empty right after the call */
    uint32_t last_queued;           /* the target's `queued` right after the last post */
    ta_result withdraw_result;
} hook_record;

static hook_record hook;

static ta_event_name_id hook_name(const ta_str *text) {
    if (text->len == 7 && memcmp(text->ptr, "post:e", 6) == 0) {
        switch (text->ptr[6]) {
        case '1': return hook.w->e1;
        case '2': return hook.w->e2;
        default: return hook.w->e3;
        }
    }
    return 0;
}

static ta_op_status dm_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    ta_str text = ((const ta_expression *)params->shared)[params->script].as.source;
    ta_system *system = hook.w->system;
    if (text.len == 8 && memcmp(text.ptr, "withdraw", 8) == 0) {
        hook.withdraw_result = ta_input_withdraw(system, hook.tokens[hook.calls - 1u]);
        return TA_OP_OK;
    }
    uint32_t at = hook.calls < HOOK_MAX ? hook.calls : HOOK_MAX - 1u;
    hook.calls += 1u;
    ta_event event = { .name = hook_name(&text), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    hook.executing[at] = ta_run_executing(system);
    uint64_t before = heap.stats.attempts;
    hook.results[at] = ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = hook.target, .event = &event,
                                                                           .time = ta_system_time(system),
                                                                           .input = &hook.tokens[at] });
    hook.attempts[at] = heap.stats.attempts - before;
    hook.admitted += hook.results[at] == TA_OK ? 1u : 0u;
    hook.run_queue_empty[at] = ta_ilist_is_empty(&system->envelopes.run);
    hook.last_queued = queued_of(hook.w, hook.target);
    return TA_OP_OK;
}
static ta_result dm_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = (void *)params->expressions;
    return TA_OK;
}
static void dm_destroy_definition(void *self, void *shared, ta_allocator unused) {
    (void)self, (void)shared, (void)unused;
}
static uint32_t dm_instance_size(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static uint32_t dm_instance_alignment(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static ta_result dm_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self, (void)params;
    return TA_OK;
}
static void dm_destroy_instance(void *self, void *shared, void *instance) {
    (void)self, (void)shared, (void)instance;
}
static ta_op_status dm_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}

static const ta_datamodel_vtable dm_vtable = {
    .name = STR("hook"),
    .prepare_definition = dm_prepare,
    .destroy_definition = dm_destroy_definition,
    .instance_size = dm_instance_size,
    .instance_alignment = dm_instance_alignment,
    .init_instance = dm_init_instance,
    .destroy_instance = dm_destroy_instance,
    .eval_condition = dm_eval_condition,
    .execute_script = dm_execute_script,
};

#define SCRIPT(text) { .kind = TA_SCXML_ACTION_SCRIPT, .as.script.script = { .kind = TA_EXPRESSION_SOURCE, .as.source = STR(text) } }

static ta_definition_id define_hooked(world *w, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(w->system, &(ta_definition_compile_params){ .spec = spec,
                                                                                .datamodel = { .v = &dm_vtable },
                                                                                .allocator = allocator,
                                                                                .out = &definition }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(w->system, definition, &id) == TA_OK);
    return id;
}

static ta_scxml_action_spec send_to(const char *event, const char *target) {
    return (ta_scxml_action_spec){
        .kind = TA_SCXML_ACTION_SEND,
        .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = { .ptr = event, .len = (uint32_t)strlen(event) } },
                     .target = { .kind = TA_SCXML_STRING_LITERAL,
                                 .as.literal = { .ptr = target, .len = (uint32_t)strlen(target) } } },
    };
}

/* The <send> target text of `actor`: "#_scxml_" and the session ID's text. */
static void session_target(world *w, ta_actor_id actor, char *out, size_t capacity) {
    char text[64];
    uint32_t needed = 0;
    ta_actor_inspection view = {0};
    CHECK(ta_actor_inspect(w->system, actor, &view) == TA_OK);
    CHECK(ta_session_id_format(w->system, &(ta_session_id_format_params){ .session = actor,
                                                                         .family = view.definition.family.name,
                                                                         .buffer = text, .capacity = sizeof text,
                                                                         .needed = &needed }) == TA_OK);
    snprintf(out, capacity, "#_scxml_%s", text);
}

/*
 * A poster's chart: `start` runs `actions` on entry and then leaves for
 * `done` by an eventless transition, so the bootstrap is two microsteps and a
 * budget of one pauses it between them.
 */
static ta_definition_id define_poster(world *w, const ta_scxml_action_spec *actions) {
    ta_scxml_handler_spec onentry[] = { { .actions = actions }, END };
    ta_scxml_transition_spec leave[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = (const ta_str[]){ STR("done"), END } }, END };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("start"), .onentry = onentry, .transitions = leave },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("done") },
        END,
    };
    return define_hooked(w, &(ta_scxml_spec){ .name = STR("poster"), .states = states });
}

/*
 * A post made by a script lands behind the mail the macrostep sent before it
 * and before the mail it sent after it: the receiver runs e1 (mail), e2 (the
 * post), e3 (mail) and so reaches s3, which only that order does. The post is
 * not on the run queue until the macrostep ends, and it is counted at once.
 */
static void test_post_inside_a_macrostep(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    char target[96];
    session_target(&w, receiver, target, sizeof target);
    ta_scxml_action_spec actions[] = { send_to("e1", target), SCRIPT("post:e2"), send_to("e3", target), END };
    ta_definition_id poster_chart = define_poster(&w, actions);
    hook = (hook_record){ .w = &w, .target = receiver };
    (void)spawn(&w, poster_chart);

    report r;
    CHECK(run(&w, true, &r) == TA_OK); /* the poster's bootstrap; then its output runs behind it */
    CHECK(hook.calls == 1u && hook.results[0] == TA_OK && hook.tokens[0] != 0);
    CHECK(hook.executing[0]);
    CHECK(hook.attempts[0] == 0u); /* inside a macrostep admission allocates nothing */
    CHECK(hook.run_queue_empty[0]);       /* invisible until the macrostep ends */
    CHECK(hook.last_queued == 2u);        /* the mail e1 and the post: counted at once */
    CHECK(r.status == TA_RUN_IDLE);
    CHECK(count_token(&r, TA_NOTICE_INPUT_TAKEN, hook.tokens[0]) == 1u);
    CHECK(count_token(&r, TA_NOTICE_INPUT_COMPLETED, hook.tokens[0]) == 1u);
    CHECK(in_state(&w, receiver, "s3"));
    CHECK(gauge(&w) == 0u && queued_of(&w, receiver) == 0u);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* A post that a script withdraws in the same macrostep never runs. */
static void test_withdraw_inside_a_macrostep(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    ta_scxml_action_spec actions[] = { SCRIPT("post:e1"), SCRIPT("withdraw"), END };
    ta_definition_id poster_chart = define_poster(&w, actions);
    hook = (hook_record){ .w = &w, .target = receiver };
    (void)spawn(&w, poster_chart);
    report r;
    CHECK(run(&w, true, &r) == TA_OK);
    CHECK(hook.results[0] == TA_OK && hook.withdraw_result == TA_OK);
    CHECK(count_token(&r, TA_NOTICE_INPUT_TAKEN, hook.tokens[0]) == 0u);
    CHECK(in_state(&w, receiver, "s0"));
    CHECK(gauge(&w) == 0u && queued_of(&w, receiver) == 0u);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* The macrostep's reserve pays for its mail and its posts alike; the post after the last envelope is FULL. */
static void test_reserve_spent_inside_a_macrostep(void) {
    ta_param_value params[] = { { TA_PARAM_MACROSTEP_RESERVE, 2 } };
    world w;
    world_init(&w, params, 1);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    ta_scxml_action_spec actions[] = { SCRIPT("post:e1"), SCRIPT("post:e2"), SCRIPT("post:e3"), END };
    ta_definition_id poster_chart = define_poster(&w, actions);
    hook = (hook_record){ .w = &w, .target = receiver };
    (void)spawn(&w, poster_chart);
    report r;
    CHECK(run(&w, false, &r) == TA_OK);
    CHECK(hook.calls == 3u && hook.results[0] == TA_OK && hook.results[1] == TA_OK && hook.results[2] == TA_ERR_FULL);
    CHECK(in_state(&w, receiver, "s2")); /* e1 and e2 ran in order; e3 was never admitted */
    CHECK(gauge(&w) == 0u);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/*
 * A macrostep that is abandoned drops what it posted, as it drops its mail:
 * the receiver never sees the input and ta_run tells the host, with the
 * token, that it was discarded.
 */
static void test_abandoned_macrostep_discards_its_posts(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    ta_scxml_action_spec actions[] = { SCRIPT("post:e1"), END };
    ta_definition_id poster_chart = define_poster(&w, actions);
    hook = (hook_record){ .w = &w, .target = receiver };
    ta_actor_id poster = spawn(&w, poster_chart);
    ta_run_status status = TA_RUN_IDLE;
    ta_run_notice notices[8];
    uint32_t count = 0;
    CHECK(ta_run(w.system, &(ta_run_params){ .budget = 1, .status = &status, .notices = notices, .notice_capacity = 8,
                                             .notice_count = &count }) == TA_OK);
    CHECK(status == TA_RUN_PAUSED && hook.calls == 1u && hook.results[0] == TA_OK);
    CHECK(gauge(&w) == 1u && queued_of(&w, receiver) == 1u);
    CHECK(ta_actor_abandon(w.system, poster) == TA_OK);
    CHECK(gauge(&w) == 0u && queued_of(&w, receiver) == 0u);
    report r;
    CHECK(run(&w, false, &r) == TA_OK);
    uint32_t found = 0;
    for (uint32_t index = 0; index < r.count; index += 1u) {
        if (r.notices[index].kind == TA_NOTICE_INPUT_REFUSED && r.notices[index].input == hook.tokens[0]) {
            CHECK(r.notices[index].reason == TA_REFUSED_DISCARDED);
            CHECK(r.notices[index].source == TA_SOURCE_HOST);
            found += 1u;
        }
    }
    CHECK(found == 1u);
    CHECK(in_state(&w, receiver, "s0"));
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* A macrostep that reaches its microstep limit is abandoned by ta_run itself: the same refusal. */
static void test_runaway_macrostep_discards_its_posts(void) {
    ta_param_value params[] = { { TA_PARAM_MICROSTEP_LIMIT, 20 } };
    world w;
    world_init(&w, params, 1);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    ta_scxml_action_spec actions[] = { SCRIPT("post:e1"), END };
    ta_scxml_handler_spec onentry[] = { { .actions = actions }, END };
    ta_scxml_transition_spec to_y[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = (const ta_str[]){ STR("y"), END } }, END };
    ta_scxml_transition_spec to_x[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = (const ta_str[]){ STR("x"), END } }, END };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("x"), .onentry = onentry, .transitions = to_y },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("y"), .transitions = to_x },
        END,
    };
    ta_definition_id spinner = define_hooked(&w, &(ta_scxml_spec){ .name = STR("spinner"), .states = states });
    hook = (hook_record){ .w = &w, .target = receiver };
    (void)spawn(&w, spinner);
    report r;
    uint32_t posted = 0, refused = 0;
    do {
        CHECK(run(&w, false, &r) == TA_OK);
        for (uint32_t index = 0; index < r.count; index += 1u) {
            if (r.notices[index].kind == TA_NOTICE_INPUT_REFUSED) {
                CHECK(r.notices[index].reason == TA_REFUSED_DISCARDED);
                refused += 1u;
            }
        }
    } while (r.status != TA_RUN_IDLE);
    posted = hook.admitted;
    CHECK(posted >= 1u && refused == posted);
    CHECK(in_state(&w, receiver, "s0"));
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 4. Invocation error reports
 */

static int invoker_starts;

static ta_op_status invoker_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
    invoker_starts += 1;
    *params->service_handle = 7;
    return TA_OP_OK;
}

static ta_op_status invoker_cancel(void *self, uint64_t service, ta_fault *fault) {
    (void)self, (void)service, (void)fault;
    return TA_OP_OK;
}

static ta_op_status invoker_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}

static const ta_invoker_vtable invoker_vtable = { .start = invoker_start, .cancel = invoker_cancel,
                                                  .forward = invoker_forward };

/*
 * `working` invokes the service "job"; e1 leaves it for `mid` (which cancels
 * the invocation); an error report in either state goes to `lost`. So `lost`
 * says an error report was applied, and `mid` that it was not (or not yet).
 */
static ta_definition_id define_invoking(world *w) {
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("working"),
          .invokes = (const ta_scxml_invoke_spec[]){
              { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("job") },
                .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") } },
              END },
          .transitions = (const ta_scxml_transition_spec[]){ ON("e1", "mid"), ON("error.communication", "lost"),
                                                             END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("mid"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("error.communication", "lost"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("lost") },
        END,
    };
    ta_definition_identity identity = { .family = { .name = STR("jobs") }, .revision = { .ptr = (const uint8_t *)"v1", .len = 2 } };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(w->system, &(ta_definition_compile_params){
                                                    .spec = &(ta_scxml_spec){ .datamodel = STR("null"),
                                                                             .identity = &identity, .states = states },
                                                    .datamodel = ta_null_datamodel(),
                                                    .allocator = allocator,
                                                    .out = &definition }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(w->system, definition, &id) == TA_OK);
    return id;
}

static void register_invoker(world *w) {
    ta_atom svc = 0;
    CHECK(ta_system_intern_atom(w->system, (ta_str)STR("svc"), &svc) == TA_OK);
    CHECK(ta_system_register_invoker(w->system, svc, (ta_invoker){ .v = &invoker_vtable }) == TA_OK);
    invoker_starts = 0;
}

/* The invocation "job" of `actor`: its ID, or 0. */
static ta_invoke_id job_of(world *w, ta_actor_id actor) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(w->system, actor, &view) == TA_OK);
    return view.invoke_count == 1u ? view.invokes[0].id : 0;
}

static uint32_t invokes_of(world *w, ta_actor_id actor) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(w->system, actor, &view) == TA_OK);
    return view.invoke_count;
}

static ta_result report_error(world *w, ta_actor_id actor, ta_invoke_id job, bool ends) {
    ta_invoke_error error = { .reason = STR("boom"), .ends = ends };
    return ta_actor_report_invoke(w->system, &(ta_invoke_report_params){ .actor = actor, .invoke_id = job,
                                                                         .kind = TA_INVOKE_REPORT_ERROR,
                                                                         .time = ta_system_time(w->system),
                                                                         .as.error = &error });
}

/*
 * A report is queued behind what was admitted before it, so the host never
 * hears BUSY: e1 runs first (the session is in `mid`, invocation cancelled by
 * leaving `working`); the report, admitted while the invocation was active, is
 * then dropped because the invocation is gone, and the chart stays in `mid`.
 */
static void test_report_for_a_cancelled_invocation_is_dropped(void) {
    world w;
    world_init(&w, NULL, 0);
    register_invoker(&w);
    ta_actor_id actor = spawn(&w, define_invoking(&w));
    settle(&w);
    CHECK(in_state(&w, actor, "working") && invoker_starts == 1);
    ta_invoke_id job = job_of(&w, actor);
    CHECK(job != 0);
    uint64_t token = 0;
    CHECK(post(&w, actor, w.e1, &token) == TA_OK);
    CHECK(report_error(&w, actor, job, false) == TA_OK); /* not BUSY: the session has an input waiting */
    CHECK(queued_of(&w, actor) == 2u);
    report r;
    CHECK(run(&w, true, &r) == TA_OK);
    CHECK(count_token(&r, TA_NOTICE_INPUT_COMPLETED, token) == 1u);
    CHECK(count_kind(&r, TA_NOTICE_INPUT_TAKEN) == 1u); /* the dropped report was never taken */
    CHECK(in_state(&w, actor, "mid"));
    CHECK(queued_of(&w, actor) == 0u);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* A report to an idle session starts a macrostep when ta_run takes it; with `ends` the invocation is retired then. */
static void test_report_is_taken_in_order(void) {
    world w;
    world_init(&w, NULL, 0);
    register_invoker(&w);
    ta_actor_id actor = spawn(&w, define_invoking(&w));
    settle(&w);
    ta_invoke_id job = job_of(&w, actor);
    CHECK(report_error(&w, actor, job, true) == TA_OK);
    CHECK(invokes_of(&w, actor) == 1u); /* retired when taken, not when admitted */
    CHECK(queued_of(&w, actor) == 1u && !ta_actor_lookup(w.system, actor)->macrostep);
    CHECK(ta_run_pending(w.system));
    report r;
    CHECK(run(&w, true, &r) == TA_OK && r.status == TA_RUN_IDLE);
    uint32_t taken = 0;
    for (uint32_t index = 0; index < r.count; index += 1u) {
        if (r.notices[index].kind == TA_NOTICE_INPUT_TAKEN) {
            taken += 1u;
            CHECK(r.notices[index].actor == actor && r.notices[index].source == TA_SOURCE_ERROR);
            CHECK(r.notices[index].input == 0u && r.notices[index].event != NULL);
            CHECK(r.notices[index].event->type == TA_EVENT_PLATFORM);
        }
    }
    CHECK(taken == 1u);
    CHECK(in_state(&w, actor, "lost"));
    CHECK(invokes_of(&w, actor) == 0u);
    CHECK(queued_of(&w, actor) == 0u);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* A report is accepted while another session's macrostep is paused, and runs after it. */
static void test_report_while_another_session_runs(void) {
    world w;
    world_init(&w, NULL, 0);
    register_invoker(&w);
    ta_actor_id actor = spawn(&w, define_invoking(&w));
    ta_actor_id other = spawn(&w, w.chain);
    settle(&w);
    ta_invoke_id job = job_of(&w, actor);
    uint64_t token = 0;
    CHECK(post(&w, other, w.e1, &token) == TA_OK);
    ta_run_status status = TA_RUN_IDLE;
    ta_run_notice notices[8];
    uint32_t count = 0;
    CHECK(ta_run(w.system, &(ta_run_params){ .budget = 1, .status = &status, .notices = notices, .notice_capacity = 8,
                                             .notice_count = &count }) == TA_OK);
    CHECK(status == TA_RUN_PAUSED && w.system->runner.slot.occupied && w.system->runner.slot.actor == other);
    CHECK(report_error(&w, actor, job, false) == TA_OK);
    CHECK(queued_of(&w, actor) == 1u);
    settle(&w);
    CHECK(in_state(&w, actor, "lost"));
    CHECK(in_state(&w, other, "s1"));
    quiescent(&w);
    world_close(&w);
}

/* An invalid report is the caller's error and queues nothing. */
static void test_report_validation(void) {
    world w;
    world_init(&w, NULL, 0);
    register_invoker(&w);
    ta_actor_id actor = spawn(&w, define_invoking(&w));
    settle(&w);
    ta_invoke_id job = job_of(&w, actor);
    ta_invoke_error bad = { .reason = STR("has.dot") };
    CHECK(ta_actor_report_invoke(w.system, &(ta_invoke_report_params){ .actor = actor, .invoke_id = job,
                                                                       .kind = TA_INVOKE_REPORT_ERROR,
                                                                       .time = ta_system_time(w.system),
                                                                       .as.error = &bad }) == TA_ERR_INVALID);
    CHECK(ta_actor_report_invoke(w.system, &(ta_invoke_report_params){ .actor = actor, .invoke_id = job + 99u,
                                                                       .kind = TA_INVOKE_REPORT_ERROR,
                                                                       .time = ta_system_time(w.system),
                                                                       .as.error = &bad }) == TA_ERR_NOT_FOUND);
    CHECK(queued_of(&w, actor) == 0u && w.system->envelopes.pool.used == 0);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 5. Inspection: ta_actor_pending and the time of a TAKEN notice
 */

/*
 * ta_actor_pending lists the waiting inputs of one session in the order
 * ta_run takes them, honours the capacity, skips withdrawn ones, counts
 * `total` from the session's queue, leaves out other sessions' inputs, and
 * allocates nothing.
 */
static void test_pending_lists_in_run_order(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.chain);
    ta_actor_id other = spawn(&w, w.chain);
    settle(&w);
    uint64_t t1 = 0, t2 = 0, t3 = 0, o1 = 0;
    CHECK(post(&w, actor, w.e1, &t1) == TA_OK);
    CHECK(post(&w, other, w.e1, &o1) == TA_OK);
    CHECK(post(&w, actor, w.e2, &t2) == TA_OK);
    CHECK(post(&w, actor, w.e3, &t3) == TA_OK);

    ta_pending_input inputs[8];
    uint32_t count = 99, total = 99;
    uint64_t before = heap.stats.attempts;
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = actor, .inputs = inputs, .capacity = 8,
                                                                 .count = &count, .total = &total }) == TA_OK);
    CHECK(heap.stats.attempts == before); /* inspection only */
    CHECK(count == 3u && total == 3u);
    CHECK(inputs[0].input == t1 && inputs[1].input == t2 && inputs[2].input == t3);
    CHECK(inputs[0].source == TA_SOURCE_HOST && inputs[0].event != NULL && inputs[0].event->name == w.e1);
    CHECK(inputs[2].event->name == w.e3);

    /* The capacity limits what is written, not the total. */
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = actor, .inputs = inputs, .capacity = 2,
                                                                 .count = &count, .total = &total }) == TA_OK);
    CHECK(count == 2u && total == 3u && inputs[1].input == t2);
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = actor, .inputs = NULL, .capacity = 0,
                                                                 .count = &count, .total = &total }) == TA_OK);
    CHECK(count == 0u && total == 3u);

    /* A withdrawn input is not listed and not counted. */
    CHECK(ta_input_withdraw(w.system, t2) == TA_OK);
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = actor, .inputs = inputs, .capacity = 8,
                                                                 .count = &count, .total = &total }) == TA_OK);
    CHECK(count == 2u && total == 2u && inputs[0].input == t1 && inputs[1].input == t3);

    /* The other session sees only its own. */
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = other, .inputs = inputs, .capacity = 8,
                                                                 .count = &count }) == TA_OK);
    CHECK(count == 1u && inputs[0].input == o1);

    /* An unknown session and bad arguments. */
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = 12345u, .inputs = inputs, .capacity = 8,
                                                                 .count = &count }) == TA_ERR_NOT_FOUND && count == 0u);
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = actor, .inputs = NULL, .capacity = 1,
                                                                 .count = &count }) == TA_ERR_INVALID);
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = actor, .inputs = inputs, .capacity = 1 }) ==
          TA_ERR_INVALID);
    CHECK(ta_actor_pending(NULL, NULL) == TA_ERR_INVALID);

    settle(&w);
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = actor, .inputs = inputs, .capacity = 8,
                                                                 .count = &count, .total = &total }) == TA_OK);
    CHECK(count == 0u && total == 0u);
    quiescent(&w);
    world_close(&w);
}

/* An error report is listed with its source and no token; inputs waiting for an address are not listed. */
static void test_pending_lists_reports_and_not_waiting_inputs(void) {
    world w;
    world_init(&w, NULL, 0);
    register_invoker(&w);
    ta_actor_id actor = spawn(&w, define_invoking(&w));
    settle(&w);
    ta_invoke_id job = job_of(&w, actor);
    uint64_t token = 0;
    CHECK(post(&w, actor, w.e1, &token) == TA_OK);
    CHECK(report_error(&w, actor, job, false) == TA_OK);
    ta_address *address = intern_address(&w, "nobody");
    uint64_t waiting = 0;
    CHECK(post_to(&w, address, w.e1, &waiting) == TA_OK);

    ta_pending_input inputs[4];
    uint32_t count = 0, total = 0;
    CHECK(ta_actor_pending(w.system, &(ta_actor_pending_params){ .actor = actor, .inputs = inputs, .capacity = 4,
                                                                 .count = &count, .total = &total }) == TA_OK);
    CHECK(count == 2u && total == 2u);
    CHECK(inputs[0].input == token && inputs[0].source == TA_SOURCE_HOST);
    CHECK(inputs[1].input == 0u && inputs[1].source == TA_SOURCE_ERROR);
    CHECK(inputs[1].event != NULL && inputs[1].event->type == TA_EVENT_PLATFORM);
    CHECK(ta_address_waiting(w.system, address) == 1u);
    CHECK(ta_input_withdraw(w.system, waiting) == TA_OK);
    ta_address_release(address);
    settle(&w);
    world_close(&w);
}

/* A TAKEN notice carries the time the input was admitted with, not the time of the take. */
static void test_taken_notice_carries_the_input_time(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.chain);
    settle(&w);
    CHECK(ta_system_set_time(w.system, 10) == TA_OK);
    uint64_t token = 0;
    CHECK(post(&w, actor, w.e1, &token) == TA_OK);
    CHECK(ta_system_set_time(w.system, 20) == TA_OK);
    report r;
    CHECK(run(&w, true, &r) == TA_OK);
    uint32_t seen = 0;
    for (uint32_t index = 0; index < r.count; index += 1u) {
        if (r.notices[index].kind == TA_NOTICE_INPUT_TAKEN) {
            CHECK(r.notices[index].input == token && r.notices[index].time == 10u);
            seen += 1u;
        } else {
            CHECK(r.notices[index].time == 0u);
        }
    }
    CHECK(seen == 1u);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 6. The input watch
 */

typedef struct {
    uint32_t marks;
    ta_actor_id last;
} watch_record;

static void watch_mark(void *self, ta_actor_id actor, ta_trace_parent *trace) {
    watch_record *record = self;
    (void)trace;
    record->marks += 1u;
    record->last = actor;
}

static const ta_input_watch_vtable watch_vtable = { .mark = watch_mark };

/* Core offers every taken input to the watch: host inputs as well as mail. */
static void test_watch_marks_host_inputs(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.chain);
    settle(&w);
    watch_record record = {0};
    CHECK(ta_system_set_input_watch(w.system, (ta_input_watch){ .self = &record, .v = &watch_vtable }) == TA_OK);
    uint64_t token = 0;
    CHECK(post(&w, actor, w.e1, &token) == TA_OK);
    CHECK(record.marks == 0u); /* admission does not offer it: taking does */
    settle(&w);
    CHECK(record.marks == 1u && record.last == actor);
    CHECK(ta_system_set_input_watch(w.system, (ta_input_watch){0}) == TA_OK);
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
    test_refusal_without_an_address();
    test_gauge_follows_every_end();
    test_post_inside_a_macrostep();
    test_withdraw_inside_a_macrostep();
    test_reserve_spent_inside_a_macrostep();
    test_abandoned_macrostep_discards_its_posts();
    test_runaway_macrostep_discards_its_posts();
    test_report_for_a_cancelled_invocation_is_dropped();
    test_report_is_taken_in_order();
    test_report_while_another_session_runs();
    test_report_validation();
    test_pending_lists_in_run_order();
    test_pending_lists_reports_and_not_waiting_inputs();
    test_taken_notice_carries_the_input_time();
    test_watch_marks_host_inputs();
    if (failures != 0) {
        fprintf(stderr, "input_test: %d checks failed\n", failures);
        return 1;
    }
    printf("input_test: ok\n");
    return 0;
}
