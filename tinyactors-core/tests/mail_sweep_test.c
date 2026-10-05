/* test: standalone asan exhaustive */
/*
 * Allocation failure at every safe point of the waiting paths (package mail,
 * ADR 0099 sections 4 and 6). A safe point is where core may allocate and an
 * out-of-memory answer changes nothing:
 *
 *   1. ta_actor_deliver with an address: the first input of a system grows
 *      the pool and rebuilds the waiting table. Fail the n-th allocation for
 *      every n the call makes: the call answers TA_ERR_NOMEM and leaves the
 *      pool, the table, the cue list and the counters as they were, and the
 *      same call made again succeeds and reaches the state of a run that never
 *      failed.
 *   2. The call that makes the address resident (ta_actor_spawn with the
 *      address): failing any of its allocations leaves the waiting list whole
 *      and unmoved; when it succeeds the list is on the run queue in order.
 *      Moving the list is its last step, and allocates nothing.
 *   3. ta_address_load_failed never allocates: with every allocation refused it
 *      still recycles the list, in order.
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

#define CHECK(condition)                                                                                      \
    do {                                                                                                      \
        if (!(condition)) {                                                                                   \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                    \
            failures += 1;                                                                                    \
        }                                                                                                     \
    } while (0)

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }
#define END {0}

static ta_test_allocator heap = { .strict = true };
static const ta_allocator allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

/* A directory that never has anyone resident until the test says so. */
typedef struct {
    ta_actor_id resident;
} fixed_directory;

static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    fixed_directory *dir = self;
    *params->out = dir->resident != 0
                       ? (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = dir->resident }
                       : (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
    return TA_OK;
}

static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

typedef struct {
    ta_system *system;
    fixed_directory directory;
    ta_definition_id chain;
    ta_event_name_id e1, e2, e3;
    ta_address *address;
} world;

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &atom) == TA_OK);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &name) == TA_OK);
    return name;
}

#define ON(event_text, target_text)                                                                 \
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR(event_text),                               \
      .targets = (const ta_str[]){ STR(target_text), END } }

/* A fresh system with the chain chart (s0 -e1-> s1 -e2-> s2 -e3-> s3) and the address "chain/n". */
static void world_init(world *w) {
    *w = (world){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &w->system) == TA_OK);
    CHECK(ta_system_set_actor_directory(w->system, (ta_actor_directory){ .self = &w->directory, .v = &dir_vtable }) ==
          TA_OK);
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
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(w->system,
                                     &(ta_definition_compile_params){
                                         .spec = &(ta_scxml_spec){ .name = STR("chain"), .states = chain },
                                         .datamodel = ta_null_datamodel(),
                                         .allocator = allocator,
                                         .out = &definition }) == TA_OK);
    CHECK(ta_system_register_definition(w->system, definition, &w->chain) == TA_OK);
    w->e1 = event_name(w->system, "e1");
    w->e2 = event_name(w->system, "e2");
    w->e3 = event_name(w->system, "e3");
    CHECK(ta_address_intern(w->system, &(ta_actor_address){ .family = { STR("chain") }, .name = STR("n") },
                            &w->address) == TA_OK);
}

static void world_close(world *w) {
    ta_address_release(w->address);
    ta_system_destroy(w->system);
    CHECK(heap.stats.live_blocks == 0);
}

static ta_result post(world *w, ta_event_name_id name, uint64_t *token) {
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(w->system, &(ta_actor_deliver_params){ .address = w->address, .event = &event,
                                                                   .time = ta_system_time(w->system),
                                                                   .input = token });
}

/* Everything the sweep compares: the pool, the quota, the lists and the table's population. */
typedef struct {
    uint32_t used, capacity, waiting;
    uint64_t quota_used, quota_reserved;
    bool pending;
} snapshot;

static snapshot snap(world *w) {
    return (snapshot){ .used = w->system->envelopes.pool.used,
                       .capacity = w->system->envelopes.pool.capacity,
                       .waiting = ta_address_waiting(w->system, w->address),
                       .quota_used = w->system->envelopes.quota.used,
                       .quota_reserved = w->system->envelopes.quota.reserved,
                       .pending = ta_run_pending(w->system) };
}

static bool same(snapshot a, snapshot b) {
    return a.used == b.used && a.capacity == b.capacity && a.waiting == b.waiting && a.quota_used == b.quota_used &&
           a.quota_reserved == b.quota_reserved && a.pending == b.pending;
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

/* One run to idle, with the notices dropped. */
static void settle(world *w) {
    CHECK(test_run_idle(w->system) == TA_OK);
}

/*
 * 1. The first input for an address, at every allocation: NOMEM leaves
 * everything as it was, and a retry reaches the state of the clean run.
 */
static void test_deliver_at_every_allocation(void) {
    world clean;
    world_init(&clean);
    uint64_t before = heap.stats.attempts;
    CHECK(post(&clean, clean.e1, NULL) == TA_OK);
    uint64_t length = heap.stats.attempts - before;
    CHECK(length > 0);
    snapshot expected = snap(&clean);
    CHECK(expected.waiting == 1u && expected.pending);
    world_close(&clean);

    for (uint64_t index = 0; index < points(length); index += 1u) {
        world w;
        world_init(&w);
        snapshot start = snap(&w);
        ta_test_allocator_fail_nth(&heap, point(index, length));
        uint64_t token = 0;
        ta_result result = post(&w, w.e1, &token);
        ta_test_allocator_fail_nth(&heap, 0);
        CHECK(result == TA_ERR_NOMEM);
        CHECK(same(snap(&w), start));
        CHECK(post(&w, w.e1, &token) == TA_OK);
        CHECK(same(snap(&w), expected));
        world_close(&w);
    }
}

/*
 * 2. Making the address resident, at every allocation of the spawn: the list
 * is whole and unmoved after a failure, in order on the run queue after
 * success, and a retry succeeds.
 */
static void test_spawn_at_every_allocation(void) {
    world clean;
    world_init(&clean);
    CHECK(post(&clean, clean.e1, NULL) == TA_OK);
    CHECK(post(&clean, clean.e2, NULL) == TA_OK);
    CHECK(post(&clean, clean.e3, NULL) == TA_OK);
    uint64_t before = heap.stats.attempts;
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(clean.system,
                         &(ta_actor_spawn_params){ .definition = clean.chain,
                                                   .address = &(ta_actor_address){ .family = { STR("chain") },
                                                                                   .name = STR("n") },
                                                   .out = &actor }) == TA_OK);
    uint64_t length = heap.stats.attempts - before;
    CHECK(length > 0 && ta_address_waiting(clean.system, clean.address) == 0u);
    world_close(&clean);

    for (uint64_t index = 0; index < points(length); index += 1u) {
        world w;
        world_init(&w);
        CHECK(post(&w, w.e1, NULL) == TA_OK);
        CHECK(post(&w, w.e2, NULL) == TA_OK);
        CHECK(post(&w, w.e3, NULL) == TA_OK);
        snapshot waiting = snap(&w);
        CHECK(waiting.waiting == 3u);
        ta_actor_spawn_params spawn = { .definition = w.chain,
                                        .address = &(ta_actor_address){ .family = { STR("chain") }, .name = STR("n") },
                                        .out = &actor };
        ta_test_allocator_fail_nth(&heap, point(index, length));
        ta_result result = ta_actor_spawn(w.system, &spawn);
        ta_test_allocator_fail_nth(&heap, 0);
        CHECK(result == TA_ERR_NOMEM);
        CHECK(same(snap(&w), waiting));
        CHECK(ta_actor_spawn(w.system, &spawn) == TA_OK);
        w.directory.resident = actor;
        CHECK(ta_address_waiting(w.system, w.address) == 0u);
        CHECK(ta_actor_lookup(w.system, actor)->queued == 3u);
        settle(&w);
        ta_state_id active[4];
        uint32_t count = 0;
        CHECK(ta_actor_active_states(w.system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                                 .capacity = 4, .count = &count }) ==
              TA_OK);
        ta_actor_inspection view = {0};
        CHECK(ta_actor_inspect(w.system, actor, &view) == TA_OK && count == 1u);
        ta_state_info info = {0};
        CHECK(ta_definition_state(w.system, &(ta_definition_state_params){ .definition = view.definition_id,
                                                                            .state = active[0], .out = &info }) == TA_OK);
        CHECK(info.id.len == 2u && memcmp(info.id.ptr, "s3", 2) == 0); /* e1, e2, e3: arrival order */
        world_close(&w);
    }
}

/* 3. Recycling a waiting list needs no memory: with every allocation refused it still succeeds, in order. */
static void test_load_failed_needs_no_memory(void) {
    world w;
    world_init(&w);
    uint64_t tokens[4] = {0};
    for (uint32_t index = 0; index < 4u; index += 1u) {
        CHECK(post(&w, w.e1, &tokens[index]) == TA_OK);
    }
    ta_test_allocator_refuse_all(&heap, true);
    uint64_t before = heap.stats.attempts;
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = w.address,
                                                                            .reason = TA_REFUSED_LOAD_FAILED }) == TA_OK);
    CHECK(heap.stats.attempts == before); /* it did not even ask */
    ta_test_allocator_refuse_all(&heap, false);
    ta_run_notice notices[8];
    uint32_t count = 0;
    ta_run_status status = TA_RUN_MORE;
    CHECK(ta_run(w.system, &(ta_run_params){ .budget = 10, .status = &status, .notices = notices,
                                             .notice_capacity = 8, .notice_count = &count }) == TA_OK);
    CHECK(status == TA_RUN_IDLE && count == 4u);
    for (uint32_t index = 0; index < count; index += 1u) {
        CHECK(notices[index].kind == TA_NOTICE_INPUT_REFUSED && notices[index].input == tokens[index]);
    }
    settle(&w);
    world_close(&w);
}

int main(void) {
    const char *requested = getenv("TA_TEST_FULL");
    full_run = requested != NULL && requested[0] != '\0' && strcmp(requested, "0") != 0;
    test_deliver_at_every_allocation();
    test_spawn_at_every_allocation();
    test_load_failed_needs_no_memory();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "mail_sweep_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("mail_sweep_test: ok\n");
    return 0;
}
