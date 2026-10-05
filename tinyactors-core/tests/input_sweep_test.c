/* test: standalone asan exhaustive */
/*
 * Allocation failure at every safe point of a host input's life (src/run.c,
 * src/invoke.c; ADR 0099 sections 5 and 8; ADR 0047 for what a failure may
 * leave). A safe point is where core may allocate and an out-of-memory answer
 * changes nothing:
 *
 *   1. ta_actor_deliver to a resident session: the pool grows. Fail the n-th
 *      allocation for every n the call makes: it answers TA_ERR_NOMEM and
 *      leaves the pool, the quota, the session's `queued` count and
 *      ta_counters.host_inputs as they were; the same call made again
 *      succeeds and the input runs.
 *   2. ta_actor_report_invoke(ERROR) at admission: the reason's event name is
 *      interned and the pool grows. The same rule.
 *   3. The take of an error report inside ta_run: the cell for the started
 *      macrostep and the session's event queue are allocated there. A failure
 *      leaves the report where it stood (ta_run answers TA_ERR_NOMEM), and
 *      the next call applies it exactly once.
 *   4. ta_input_withdraw never allocates: with every allocation refused it
 *      still withdraws.
 *
 * The fast pass fails three of the n (the first, the middle, the last);
 * TA_TEST_FULL=1 fails every one (`mise run test --full`), per AGENTS.md.
 *
 * Includes the amalgamation, because the package is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_allocator.h"
#include "run_support.h"

static int failures;
static bool full_run; /* TA_TEST_FULL=1 asks for every failure point; read once at the top of main */

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

/* The service: it only has to start. */
static ta_op_status invoker_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
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
 * `working` invokes "job" and goes to `moved` on e1 and to `lost` on any
 * error.communication; `moved` ignores e1 and also goes to `lost`.
 */
typedef struct {
    ta_system *system;
    ta_definition_id jobs;
    ta_event_name_id e1;
    ta_actor_id actor;
    ta_invoke_id job;
} world;

/*
 * A small pool: 512 envelopes in pages of 256, a reserve of 1 and a watermark
 * of 99%, so that the admission after the first page is used up is the first
 * to have to grow the pool. A pool of the default size would take a thousand
 * admissions to get there.
 */
static void world_init(world *w) {
    *w = (world){0};
    ta_param_value params[] = { { TA_PARAM_POOL_ENVELOPES, 512 }, { TA_PARAM_MACROSTEP_RESERVE, 1 },
                                { TA_PARAM_ADMISSION_WATERMARK, 99 } };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params, .param_count = 3 },
                           &w->system) == TA_OK);
    ta_atom svc = 0;
    CHECK(ta_system_intern_atom(w->system, (ta_str)STR("svc"), &svc) == TA_OK);
    CHECK(ta_system_register_invoker(w->system, svc, (ta_invoker){ .v = &invoker_vtable }) == TA_OK);
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("working"),
          .invokes = (const ta_scxml_invoke_spec[]){
              { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("job") },
                .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") } },
              END },
          .transitions = (const ta_scxml_transition_spec[]){ ON("e1", "moved"), ON("error.communication", "lost"),
                                                             END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("moved"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("error.communication", "lost"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("lost") },
        END,
    };
    ta_definition_identity identity = { .family = { .name = STR("jobs") },
                                        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 } };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(w->system, &(ta_definition_compile_params){
                                                    .spec = &(ta_scxml_spec){ .datamodel = STR("null"),
                                                                             .identity = &identity, .states = states },
                                                    .datamodel = ta_null_datamodel(),
                                                    .allocator = allocator,
                                                    .out = &definition }) == TA_OK);
    CHECK(ta_system_register_definition(w->system, definition, &w->jobs) == TA_OK);
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(w->system, (ta_str)STR("e1"), &atom) == TA_OK);
    CHECK(ta_event_name_intern(w->system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &w->e1) == TA_OK);
    CHECK(ta_actor_spawn(w->system, &(ta_actor_spawn_params){ .definition = w->jobs, .out = &w->actor }) == TA_OK);
    CHECK(test_run_idle(w->system) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(w->system, w->actor, &view) == TA_OK && view.invoke_count == 1u);
    w->job = view.invokes[0].id;
}

static void world_close(world *w) {
    ta_system_destroy(w->system);
    CHECK(heap.stats.live_blocks == 0);
}

static ta_result post(world *w, uint64_t *token) {
    ta_event event = { .name = w->e1, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(w->system, &(ta_actor_deliver_params){ .actor = w->actor, .event = &event,
                                                                   .time = ta_system_time(w->system),
                                                                   .input = token });
}

static ta_result report(world *w) {
    ta_invoke_error error = { .reason = STR("boom"), .ends = true };
    return ta_actor_report_invoke(w->system, &(ta_invoke_report_params){ .actor = w->actor, .invoke_id = w->job,
                                                                         .kind = TA_INVOKE_REPORT_ERROR,
                                                                         .time = ta_system_time(w->system),
                                                                         .as.error = &error });
}

static bool in_state(world *w, const char *id) {
    ta_state_id active[8];
    uint32_t count = 0;
    ta_actor_inspection inspection = {0};
    CHECK(ta_actor_inspect(w->system, w->actor, &inspection) == TA_OK);
    CHECK(ta_actor_active_states(w->system, &(ta_actor_active_states_params){ .actor = w->actor, .states = active,
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

/* What an operation that failed must leave as it found it. */
typedef struct {
    uint32_t used, capacity;
    uint32_t quota_used, quota_reserved;
    uint32_t queued;
    uint64_t gauge;
    bool pending;
} snapshot;

static snapshot snap(world *w) {
    ta_counters counters;
    ta_system_counters(w->system, &counters);
    return (snapshot){ .used = w->system->envelopes.pool.used,
                       .capacity = ta_pool_extent(&w->system->envelopes.pool),
                       .quota_used = w->system->envelopes.quota.used,
                       .quota_reserved = w->system->envelopes.quota.reserved,
                       .queued = ta_actor_lookup(w->system, w->actor)->queued,
                       .gauge = counters.host_inputs,
                       .pending = ta_run_pending(w->system) };
}

/* Everything but the pool's capacity: a failed refill may leave pages it did add. */
static bool same(snapshot a, snapshot b) {
    return a.used == b.used && a.quota_used == b.quota_used && a.quota_reserved == b.quota_reserved &&
           a.queued == b.queued && a.gauge == b.gauge && a.pending == b.pending;
}

/* The failure points to try for an operation that makes `length` allocations: three, or all. */
static uint64_t point(uint64_t index, uint64_t length) {
    if (full_run) {
        return index + 1u;
    }
    return index == 0 ? 1u : index == 1 ? (length + 1u) / 2u : length;
}

static uint64_t points(uint64_t length) {
    return full_run ? length : (length < 3u ? length : 3u);
}

static void settle(world *w) {
    CHECK(test_run_idle(w->system) == TA_OK);
    ta_run_status status = TA_RUN_IDLE;
    ta_run_notice notices[8];
    uint32_t count = 0;
    CHECK(ta_run(w->system, &(ta_run_params){ .budget = 1000, .status = &status, .notices = notices,
                                              .notice_capacity = 8, .notice_count = &count }) == TA_OK);
}

typedef ta_result (*operation)(world *);

static ta_result post_one(world *w) {
    uint64_t token = 0;
    return post(w, &token);
}

/*
 * How many times `op` succeeds in a fresh world without allocating before the
 * first call that does, and how many allocations that call makes. The sweeps
 * run the calls before it, then fail each allocation of the next.
 */
typedef struct {
    uint32_t free_calls;
    uint64_t length;
} growth;

static growth find_growth(operation op) {
    world clean;
    world_init(&clean);
    growth found = {0};
    for (uint32_t calls = 0; calls < 400u && found.length == 0; calls += 1u) {
        uint64_t before = heap.stats.attempts;
        CHECK(op(&clean) == TA_OK);
        found.length = heap.stats.attempts - before;
        found.free_calls = calls;
    }
    CHECK(found.length > 0);
    world_close(&clean);
    return found;
}

/*
 * 1 and 2. Admission (a host input to a resident session, an error report) at
 * every allocation of the call that grows the pool: NOMEM leaves everything
 * as it was, and a retry succeeds and the input runs.
 */
static void sweep_admission(operation op, const char *settled) {
    growth found = find_growth(op);
    for (uint64_t index = 0; index < points(found.length); index += 1u) {
        world w;
        world_init(&w);
        for (uint32_t calls = 0; calls < found.free_calls; calls += 1u) {
            CHECK(op(&w) == TA_OK);
        }
        snapshot start = snap(&w);
        ta_test_allocator_fail_nth(&heap, point(index, found.length));
        ta_result result = op(&w);
        ta_test_allocator_fail_nth(&heap, 0);
        CHECK(result == TA_ERR_NOMEM);
        CHECK(same(snap(&w), start));
        CHECK(op(&w) == TA_OK);
        CHECK(snap(&w).queued == start.queued + 1u);
        settle(&w);
        CHECK(in_state(&w, settled));
        CHECK(w.system->envelopes.pool.used == 0);
        world_close(&w);
    }
}

static void test_deliver_at_every_allocation(void) {
    sweep_admission(post_one, "moved");
}

static void test_report_admission_at_every_allocation(void) {
    sweep_admission(report, "lost");
}

/*
 * 3. The take of a report, at every allocation of the ta_run that applies it
 * (the cell for the started macrostep, the session's queue, and then the
 * macrostep's own scratch): ta_run stops, and a failure before the report was
 * applied leaves it where it stood, so the next call applies it exactly once.
 */
static void test_report_take_at_every_allocation(void) {
    world clean;
    world_init(&clean);
    CHECK(report(&clean) == TA_OK);
    ta_run_status status = TA_RUN_IDLE;
    ta_run_notice notices[8];
    uint32_t count = 0;
    uint64_t before = heap.stats.attempts;
    CHECK(ta_run(clean.system, &(ta_run_params){ .budget = 1000, .status = &status, .notices = notices,
                                                 .notice_capacity = 8, .notice_count = &count }) == TA_OK);
    uint64_t length = heap.stats.attempts - before;
    CHECK(in_state(&clean, "lost"));
    world_close(&clean);

    for (uint64_t index = 0; index < points(length); index += 1u) {
        world w;
        world_init(&w);
        CHECK(report(&w) == TA_OK);
        snapshot start = snap(&w);
        ta_test_allocator_fail_nth(&heap, point(index, length));
        ta_result result = ta_run(w.system, &(ta_run_params){ .budget = 1000, .status = &status, .notices = notices,
                                                              .notice_capacity = 8, .notice_count = &count });
        ta_test_allocator_fail_nth(&heap, 0);
        if (result == TA_ERR_NOMEM) {
            CHECK(status == TA_RUN_MORE);
            snapshot stopped = snap(&w);
            CHECK(stopped.queued == start.queued && stopped.gauge == start.gauge);
            CHECK(!in_state(&w, "lost")); /* nothing was applied */
        }
        settle(&w);
        CHECK(in_state(&w, "lost"));
        ta_actor_inspection view;
        CHECK(ta_actor_inspect(w.system, w.actor, &view) == TA_OK && view.invoke_count == 0u);
        CHECK(w.system->envelopes.pool.used == 0);
        world_close(&w);
    }
}

/* 4. A withdrawal makes no allocation: it works with every allocation refused. */
static void test_withdraw_needs_no_memory(void) {
    world w;
    world_init(&w);
    uint64_t token = 0;
    CHECK(post(&w, &token) == TA_OK);
    ta_test_allocator_fail_from_nth(&heap, 1u);
    CHECK(ta_input_withdraw(w.system, token) == TA_OK);
    CHECK(ta_input_withdraw(w.system, token) == TA_ERR_NOT_FOUND);
    ta_test_allocator_fail_from_nth(&heap, 0);
    CHECK(snap(&w).gauge == 0u && snap(&w).queued == 0u);
    settle(&w);
    CHECK(in_state(&w, "working"));
    world_close(&w);
}

int main(void) {
    const char *requested = getenv("TA_TEST_FULL");
    full_run = requested != NULL && requested[0] != '\0' && strcmp(requested, "0") != 0;
    test_deliver_at_every_allocation();
    test_report_admission_at_every_allocation();
    test_report_take_at_every_allocation();
    test_withdraw_needs_no_memory();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "input_sweep_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("input_sweep_test: ok\n");
    return 0;
}
