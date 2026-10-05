/* test: runtime asan */
/*
 * ta_run and admission (ADR 0099 sections 3 to 8), through the public header:
 * inputs run in the order they were admitted, whoever they are for; the
 * budget is for the whole call; a full notice array stops core between
 * inputs and loses nothing; the pool refuses host input at its watermark
 * with TA_ERR_FULL and admits again once ta_run has made room; an input for a
 * session that finished or went away is reported by a REFUSED notice, not
 * lost; and the tokens ta_actor_deliver() hands out name the notices.
 *
 * Uses the C library; the core does not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                                 \
        }                                                                                  \
    } while (0)

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define END {0}

static ta_test_allocator heap;
static const ta_allocator allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

/* ------------------------------------------------------------------------
 * A system with two charts: `toggle`, which flips between a and b on `go`,
 * and `finisher`, which reaches a final state on `end`.
 */

typedef struct {
    ta_system *system;
    ta_definition_id toggle;
    ta_definition_id finisher;
    ta_event_name_id go;
    ta_event_name_id end;
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

static void world_init(world *w, const ta_param_value *params, uint32_t param_count) {
    *w = (world){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params, .param_count = param_count },
                           &w->system) == TA_OK);
    ta_scxml_state_spec toggle_states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"),
          .transitions = (const ta_scxml_transition_spec[]){
              { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = (const ta_str[]){ STR("b"), END } },
              END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"),
          .transitions = (const ta_scxml_transition_spec[]){
              { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = (const ta_str[]){ STR("a"), END } },
              END } },
        END,
    };
    ta_scxml_state_spec finisher_states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("wait"),
          .transitions = (const ta_scxml_transition_spec[]){
              { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("end"), .targets = (const ta_str[]){ STR("over"), END } },
              END } },
        { .kind = TA_SCXML_STATE_FINAL, .id = STR("over") },
        END,
    };
    w->toggle = define(w->system, &(ta_scxml_spec){ .name = STR("toggle"), .states = toggle_states });
    w->finisher = define(w->system, &(ta_scxml_spec){ .name = STR("finisher"), .states = finisher_states });
    w->go = event_name(w->system, "go");
    w->end = event_name(w->system, "end");
}

static void world_close(world *w) {
    ta_system_destroy(w->system);
}

static ta_actor_id spawn(world *w, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(w->system, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    return actor;
}

/* Admits `name` for `actor`; the token comes back in `input`. */
static ta_result post(world *w, ta_actor_id actor, ta_event_name_id name, uint64_t *input) {
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(w->system, &(ta_actor_deliver_params){ .actor = actor, .event = &event,
                                                                   .time = ta_system_time(w->system), .input = input });
}

/* One ta_run call; answers its result and fills what it reported. */
typedef struct {
    ta_run_status status;
    ta_run_notice notices[32];
    uint32_t count;
} report;

static ta_result run(world *w, uint32_t budget, uint32_t capacity, bool taken, report *out) {
    out->count = 0;
    return ta_run(w->system, &(ta_run_params){ .budget = budget,
                                               .report_taken = taken,
                                               .status = &out->status,
                                               .notices = out->notices,
                                               .notice_capacity = capacity,
                                               .notice_count = &out->count });
}

/* Counts the notices of `kind` in `r`. */
static uint32_t count_kind(const report *r, ta_run_notice_kind kind) {
    uint32_t n = 0;
    for (uint32_t index = 0; index < r->count; index += 1u) {
        n += r->notices[index].kind == kind ? 1u : 0u;
    }
    return n;
}

/* Runs until IDLE, appending the COMPLETED tokens to `tokens`; answers how many calls it took. */
static uint32_t drain(world *w, uint32_t budget, uint32_t capacity, uint64_t *tokens, uint32_t *token_count) {
    uint32_t calls = 0;
    report r;
    do {
        CHECK(run(w, budget, capacity, false, &r) == TA_OK);
        calls += 1u;
        for (uint32_t index = 0; index < r.count; index += 1u) {
            if (r.notices[index].kind == TA_NOTICE_INPUT_COMPLETED) {
                tokens[*token_count] = r.notices[index].input;
                *token_count += 1u;
            }
        }
        CHECK(calls < 1000u);
    } while (r.status != TA_RUN_IDLE && calls < 1000u);
    return calls;
}

/* ------------------------------------------------------------------------
 * Tests
 */

/* Nothing admitted: IDLE and no notices; bad arguments are caller errors. */
static void test_idle_and_arguments(void) {
    world w;
    world_init(&w, NULL, 0);
    report r;
    r.status = TA_RUN_MORE;
    CHECK(run(&w, 10, 8, false, &r) == TA_OK);
    CHECK(r.status == TA_RUN_IDLE && r.count == 0);

    ta_run_status status = TA_RUN_MORE;
    CHECK(ta_run(w.system, &(ta_run_params){ .budget = 0, .status = &status }) == TA_ERR_INVALID);
    CHECK(run(&w, 10, 2, false, &r) == TA_ERR_INVALID); /* room for less than one input's notices */
    world_close(&w);
}

/* Inputs run in the order they were admitted, across actors; each notice names its token. */
static void test_order_and_tokens(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id first = spawn(&w, w.toggle);
    ta_actor_id second = spawn(&w, w.toggle);
    uint64_t tokens[8] = {0};
    CHECK(post(&w, second, w.go, &tokens[0]) == TA_OK);
    CHECK(post(&w, first, w.go, &tokens[1]) == TA_OK);
    CHECK(post(&w, second, w.go, &tokens[2]) == TA_OK);
    CHECK(tokens[0] != 0 && tokens[1] != 0 && tokens[2] != 0);
    CHECK(tokens[0] != tokens[1] && tokens[1] != tokens[2]);

    report r;
    CHECK(run(&w, 100, 32, true, &r) == TA_OK);
    CHECK(r.status == TA_RUN_IDLE);
    /* Two bootstraps, then the three events: the events are taken, and all five inputs complete. */
    CHECK(count_kind(&r, TA_NOTICE_INPUT_TAKEN) == 3); /* a bootstrap has no event to be taken with */
    CHECK(count_kind(&r, TA_NOTICE_INPUT_COMPLETED) == 5);
    uint64_t completed[8];
    uint32_t seen = 0;
    for (uint32_t index = 0; index < r.count; index += 1u) {
        if (r.notices[index].kind == TA_NOTICE_INPUT_COMPLETED && r.notices[index].input != 0) {
            completed[seen] = r.notices[index].input;
            seen += 1u;
            CHECK(r.notices[index].status == TA_INPUT_STABLE);
        }
    }
    CHECK(seen == 3 && completed[0] == tokens[0] && completed[1] == tokens[1] && completed[2] == tokens[2]);
    world_close(&w);
}

/* The budget is for the whole call; a small one makes MORE, and the rest follows. */
static void test_budget_is_for_the_call(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.toggle);
    uint64_t tokens[8] = {0};
    for (uint32_t index = 0; index < 5; index += 1u) {
        CHECK(post(&w, actor, w.go, &tokens[index]) == TA_OK);
    }
    report r;
    CHECK(run(&w, 1, 32, false, &r) == TA_OK);
    /* One microstep ends the call: inside the bootstrap's macrostep, which has not ended yet, or between inputs. */
    CHECK(r.status == TA_RUN_MORE || r.status == TA_RUN_PAUSED);
    CHECK(count_kind(&r, TA_NOTICE_INPUT_COMPLETED) <= 1);
    uint64_t completed[8];
    uint32_t seen = 0;
    uint32_t calls = drain(&w, 1, 32, completed, &seen);
    CHECK(calls >= 4);
    CHECK(seen >= 4); /* the bootstrap may have been in the first call */
    world_close(&w);
}

/* A notice array that cannot take another input's notices stops core between inputs, and nothing is lost. */
static void test_notices_full(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.toggle);
    uint64_t tokens[8] = {0};
    for (uint32_t index = 0; index < 6; index += 1u) {
        CHECK(post(&w, actor, w.go, &tokens[index]) == TA_OK);
    }
    report r;
    CHECK(run(&w, 100, 3, false, &r) == TA_OK);
    CHECK(r.status == TA_RUN_NOTICES_FULL);
    uint64_t completed[16];
    uint32_t seen = 0;
    for (uint32_t index = 0; index < r.count; index += 1u) {
        if (r.notices[index].kind == TA_NOTICE_INPUT_COMPLETED && r.notices[index].input != 0) {
            completed[seen] = r.notices[index].input;
            seen += 1u;
        }
    }
    drain(&w, 100, 3, completed, &seen);
    uint32_t matched = 0;
    for (uint32_t index = 0; index < seen; index += 1u) {
        if (completed[index] != 0 && matched < 6 && completed[index] == tokens[matched]) {
            matched += 1u;
        }
    }
    CHECK(matched == 6);
    world_close(&w);
}

/* The pool refuses host input at its watermark, and admits again once ta_run has made room. */
static void test_full_and_room(void) {
    ta_param_value params[] = {
        { .param = TA_PARAM_POOL_ENVELOPES, .value = 256 },
        { .param = TA_PARAM_ADMISSION_WATERMARK, .value = 50 },
        { .param = TA_PARAM_MACROSTEP_RESERVE, .value = 8 },
    };
    world w;
    world_init(&w, params, 3);
    ta_actor_id actor = spawn(&w, w.toggle);
    uint32_t admitted = 0;
    ta_result result = TA_OK;
    while (admitted < 1000u && (result = post(&w, actor, w.go, NULL)) == TA_OK) {
        admitted += 1u;
    }
    CHECK(result == TA_ERR_FULL);
    CHECK(admitted > 0 && admitted < 256);

    /* Refusal retains nothing and leaves the pool as it was: the same offer is refused again. */
    CHECK(post(&w, actor, w.go, NULL) == TA_ERR_FULL);

    uint64_t completed[512];
    uint32_t seen = 0;
    drain(&w, 100, 8, completed, &seen);
    CHECK(seen >= admitted);
    CHECK(post(&w, actor, w.go, NULL) == TA_OK);
    drain(&w, 100, 8, completed, &seen);
    world_close(&w);
}

/* A session that finishes with inputs behind it: the rest are REFUSED (done), and new ones are TA_ERR_DONE. */
static void test_finished_session(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.finisher);
    uint64_t tokens[3] = {0};
    CHECK(post(&w, actor, w.end, &tokens[0]) == TA_OK);
    CHECK(post(&w, actor, w.end, &tokens[1]) == TA_OK);
    CHECK(post(&w, actor, w.end, &tokens[2]) == TA_OK);
    report r;
    CHECK(run(&w, 100, 32, false, &r) == TA_OK);
    CHECK(r.status == TA_RUN_IDLE);
    CHECK(count_kind(&r, TA_NOTICE_FINISHED) == 1);
    bool done = false;
    uint32_t refused = 0;
    for (uint32_t index = 0; index < r.count; index += 1u) {
        const ta_run_notice *notice = &r.notices[index];
        if (notice->kind == TA_NOTICE_INPUT_COMPLETED && notice->input == tokens[0]) {
            done = notice->status == TA_INPUT_DONE;
        }
        if (notice->kind == TA_NOTICE_INPUT_REFUSED) {
            refused += 1u;
            CHECK(notice->reason == TA_REFUSED_DONE);
            CHECK(notice->input == tokens[1] || notice->input == tokens[2]);
            CHECK(notice->event != NULL && notice->event->name == w.end);
        }
    }
    CHECK(done && refused == 2);
    CHECK(post(&w, actor, w.end, NULL) == TA_ERR_DONE);
    world_close(&w);
}

/* A session destroyed with inputs behind it: each is REFUSED (destroyed), and the ID is not found. */
static void test_destroyed_session(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.toggle);
    report r;
    CHECK(run(&w, 100, 32, false, &r) == TA_OK); /* the bootstrap */
    uint64_t tokens[2] = {0};
    CHECK(post(&w, actor, w.go, &tokens[0]) == TA_OK);
    CHECK(post(&w, actor, w.go, &tokens[1]) == TA_OK);
    CHECK(ta_actor_destroy(w.system, actor) == TA_OK);
    CHECK(post(&w, actor, w.go, NULL) == TA_ERR_NOT_FOUND);
    CHECK(run(&w, 100, 32, false, &r) == TA_OK);
    CHECK(r.status == TA_RUN_IDLE);
    uint32_t refused = 0;
    for (uint32_t index = 0; index < r.count; index += 1u) {
        const ta_run_notice *notice = &r.notices[index];
        if (notice->kind == TA_NOTICE_INPUT_REFUSED) {
            refused += 1u;
            CHECK(notice->reason == TA_REFUSED_DESTROYED);
            CHECK(notice->input == tokens[0] || notice->input == tokens[1]);
        }
    }
    CHECK(refused == 2);
    world_close(&w);
}

/* A malformed event is a caller error whatever the actor's state, and retains nothing. */
static void test_malformed(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id actor = spawn(&w, w.toggle);
    CHECK(ta_actor_deliver(w.system, &(ta_actor_deliver_params){ .actor = actor, .event = NULL }) == TA_ERR_INVALID);
    ta_event never = { .name = 0x7ffffff0u, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(w.system, &(ta_actor_deliver_params){ .actor = actor, .event = &never,
                                                                 .time = ta_system_time(w.system) }) == TA_ERR_INVALID);
    report r;
    CHECK(run(&w, 100, 32, false, &r) == TA_OK);
    CHECK(r.status == TA_RUN_IDLE);
    CHECK(count_kind(&r, TA_NOTICE_INPUT_REFUSED) == 0);
    world_close(&w);
}

/* test_steady_state_makes_no_allocator_call: once a few rounds have grown the envelope pool, the started-cell pool
 * and the waiting tables to what the load needs, deliver-and-run rounds against 20 of 2000 actors make no allocator
 * call at all. This is the property napi's scale.js reports as "steady-state allocator calls", pinned here in counts
 * without a clock: a refill that rebuilt a table sized by the population, a trim and refill of a pool, or a scratch
 * buffer per call would show as attempts. The population (2000 sessions) is far larger than the 20 that receive
 * mail, so work that scaled with it would be seen. */
static void test_steady_state_makes_no_allocator_call(void) {
    enum { ACTORS = 2000, ACTIVE = 20, WARMUP = 50, ROUNDS = 1000 };
    world w;
    world_init(&w, NULL, 0);
    static ta_actor_id actors[ACTORS];
    for (uint32_t index = 0; index < ACTORS; index += 1u) {
        actors[index] = spawn(&w, w.toggle);
    }
    static uint64_t tokens[ACTORS + 8];
    uint32_t seen = 0;
    (void)drain(&w, 64, 32, tokens, &seen); /* bootstraps everyone */
    CHECK(seen == ACTORS);
    uint64_t before = 0;
    for (uint32_t round = 0; round < WARMUP + ROUNDS; round += 1u) {
        if (round == WARMUP) {
            before = heap.stats.attempts;
        }
        for (uint32_t index = 0; index < ACTIVE; index += 1u) {
            uint64_t input = 0;
            CHECK(post(&w, actors[index * (ACTORS / ACTIVE)], w.go, &input) == TA_OK);
        }
        seen = 0;
        uint32_t calls = drain(&w, 64, 32, tokens, &seen);
        CHECK(seen == ACTIVE);
        CHECK(calls <= 2u); /* ACTIVE inputs fit one call's budget and notices: one call to run, one to see IDLE */
    }
    CHECK(heap.stats.attempts == before);
    world_close(&w);
}

int main(void) {
    test_idle_and_arguments();
    test_order_and_tokens();
    test_budget_is_for_the_call();
    test_notices_full();
    test_full_and_room();
    test_finished_session();
    test_destroyed_session();
    test_malformed();
    test_steady_state_makes_no_allocator_call();
    CHECK(heap.stats.live_blocks == 0);
    if (failures != 0) {
        fprintf(stderr, "run test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core run test passed");
    return 0;
}
