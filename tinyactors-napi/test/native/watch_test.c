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
    uint64_t definitions[] = { 0, 7, UINT64_MAX };
    CHECK(tna_trace_watch_set(&watch, &(tna_trace_watch_set_params){ .sessions = sessions, .session_count = 5001,
                                                                      .definitions = definitions,
                                                                      .definition_count = 3 }) == TA_OK);
    CHECK(!tna_trace_watch_empty(&watch));
    CHECK(watch.sessions.count == 5000);
    CHECK(watch.definitions.count == 3);
    for (uint32_t at = 0; at < 5000; at += 1u) {
        CHECK(tna_id_set_has(&watch.sessions, sessions[at]));
        CHECK(!tna_id_set_has(&watch.sessions, sessions[at] + 1u));
    }
    CHECK(tna_id_set_has(&watch.definitions, 0));
    CHECK(tna_id_set_has(&watch.definitions, 7));
    CHECK(tna_id_set_has(&watch.definitions, UINT64_MAX));
    CHECK(!tna_id_set_has(&watch.definitions, 8));
    CHECK(!tna_id_set_has(&watch.sessions, UINT64_MAX));

    /* Replacing with nothing empties it and frees the sets. */
    CHECK(tna_trace_watch_set(&watch, &(tna_trace_watch_set_params){0}) == TA_OK);
    CHECK(tna_trace_watch_empty(&watch));
    CHECK(!tna_id_set_has(&watch.sessions, sessions[0]));
    tna_trace_watch_destroy(&watch);
    CHECK(tna_allocator_total(&allocator).bytes_in_use == 0);
}

int main(void) {
    sets_answer_membership();
    puts("watch_test passed");
    return 0;
}
