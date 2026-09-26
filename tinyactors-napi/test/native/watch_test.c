#include <stdio.h>
#include <stdint.h>

#include "allocator.h"
#include "check.h"
#include "watch.h"

/* Membership is exact at sizes past the old 1024 limit, duplicates count
 * once, and UINT64_MAX (the empty marker) is still an ID. */
static void sets_answer_membership(void) {
    tna_allocator allocator;
    tna_allocator_init(&allocator);
    tna_trace_watch watch;
    tna_trace_watch_init(&watch, tna_allocator_for(&allocator, TNA_MEMORY_OTHER));
    CHECK(tna_trace_watch_empty(&watch));

    static uint64_t sessions[5001];
    for (uint32_t at = 0; at < 5000; at += 1u) {
        sessions[at] = (uint64_t)at * 3u + ((uint64_t)1 << 32);
    }
    sessions[5000] = sessions[0]; /* a duplicate */
    CHECK(tna_trace_watch_set(&watch, &(tna_trace_watch_set_params){ .sessions = sessions,
                                                                      .session_count = 5001 }) == TA_OK);
    CHECK(!tna_trace_watch_empty(&watch));
    CHECK(watch.sessions.count == 5000);
    for (uint32_t at = 0; at < 5000; at += 1u) {
        CHECK(tna_id_set_has(&watch.sessions, sessions[at]));
        CHECK(!tna_id_set_has(&watch.sessions, sessions[at] + 1u));
    }
    CHECK(!tna_id_set_has(&watch.sessions, UINT64_MAX));
    uint64_t edge[] = { 0, 7, UINT64_MAX };
    CHECK(tna_trace_watch_set(&watch, &(tna_trace_watch_set_params){ .sessions = edge, .session_count = 3 }) ==
          TA_OK);
    CHECK(tna_id_set_has(&watch.sessions, 0) && tna_id_set_has(&watch.sessions, 7));
    CHECK(tna_id_set_has(&watch.sessions, UINT64_MAX) && !tna_id_set_has(&watch.sessions, 8));

    /* Replacing with nothing empties it and frees the sets. */
    CHECK(tna_trace_watch_set(&watch, &(tna_trace_watch_set_params){0}) == TA_OK);
    CHECK(tna_trace_watch_empty(&watch));
    CHECK(!tna_id_set_has(&watch.sessions, sessions[0]));
    tna_trace_watch_destroy(&watch);
    CHECK(tna_allocator_total(&allocator).bytes_in_use == 0);
}

static ta_definition_id define_family(ta_system *system, ta_allocator allocator, const char *family, uint32_t length) {
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = { "idle", 4 } },
        {0},
    };
    ta_definition_identity identity = { .family = { { family, length } } };
    ta_definition *definition = NULL;
    ta_definition_id id = 0;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                 .spec = &(ta_scxml_spec){ .identity = &identity, .states = states },
                                                 .datamodel = ta_null_datamodel(),
                                                 .allocator = allocator,
                                                 .out = &definition }) == TA_OK);
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static bool marked(tna_trace_watch *watch, ta_system *system, ta_actor_id actor) {
    ta_trace_parent trace = {0};
    tna_trace_watch_mark(watch, system, actor, &trace);
    return (trace.context.detail & TA_TRACE_DETAIL_WATCHED) != 0;
}

/* A watched family is resolved when a delivery first meets a definition, so
 * a definition registered after the watch is caught (core ADR 0078). */
static void families_resolve_as_met(void) {
    tna_allocator allocator;
    tna_allocator_init(&allocator);
    ta_allocator core_allocator = tna_allocator_for(&allocator, TNA_MEMORY_CORE);
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = core_allocator }, &system) == TA_OK);
    tna_trace_watch watch;
    tna_trace_watch_init(&watch, tna_allocator_for(&allocator, TNA_MEMORY_OTHER));
    ta_str families[] = { { "order/invoke", 12 } };
    CHECK(tna_trace_watch_set(&watch, &(tna_trace_watch_set_params){ .families = families, .family_count = 1 }) ==
          TA_OK);
    CHECK(!tna_trace_watch_empty(&watch));

    ta_definition_id other = define_family(system, core_allocator, "order", 5);
    ta_definition_id child = define_family(system, core_allocator, "order/invoke", 12);
    ta_actor_id plain = 0;
    ta_actor_id watched = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = other, .out = &plain }) == TA_OK);
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = child, .out = &watched }) == TA_OK);
    CHECK(marked(&watch, system, watched));
    CHECK(!marked(&watch, system, plain));
    /* Asked again, the answers come from the sets. */
    CHECK(marked(&watch, system, watched));
    CHECK(!marked(&watch, system, plain));
    CHECK(watch.checked.count == 2 && watch.matched.count == 1);

    tna_trace_watch_destroy(&watch);
    ta_system_destroy(system);
    CHECK(tna_allocator_total(&allocator).bytes_in_use == 0);
}

int main(void) {
    sets_answer_membership();
    families_resolve_as_met();
    puts("watch_test passed");
    return 0;
}
