#pragma once
/*
 * Watched traces (napi ADR 0017 section 3): the sessions and definitions
 * an operator asked to see in full. The scheduler consults the watch at
 * each delivery it makes to core, which is every post, send, timer and
 * routed message. A matching delivery gets a sampled context marked
 * TA_TRACE_DETAIL_WATCHED (core ADR 0073), and core carries the mark to
 * everything the macrostep causes. With no watch, the check is one branch.
 *
 * This module has no Node-API in it.
 */
#include <stdbool.h>
#include <stdint.h>

#include "tinyactors.h"

typedef struct tah_id_set tah_id_set;

typedef struct tah_trace_watch tah_trace_watch;

void tah_trace_watch_init(tah_trace_watch *watch, ta_allocator allocator);
void tah_trace_watch_destroy(tah_trace_watch *watch);

typedef struct {
    const uint64_t *sessions;
    uint32_t session_count;
    const ta_str *families;
    uint32_t family_count;
} tah_trace_watch_set_params;

/* Replaces what is watched. TA_ERR_NOMEM leaves the watch as it was. */
ta_result tah_trace_watch_set(tah_trace_watch *watch, const tah_trace_watch_set_params *params);

/* How big the watch is (napi's traceWatchSizes(), for tests): IDs in each
 * set, and the slots `matched` and `checked` hold together. */
typedef struct {
    uint32_t sessions;
    uint32_t matched;
    uint32_t checked;
    uint32_t capacity;
} tah_trace_watch_sizes;
tah_trace_watch_sizes tah_trace_watch_measure(const tah_trace_watch *watch);

/* Drops a definition core has freed from `matched` and `checked` (core ADR 0079). */
void tah_trace_watch_forget_definition(tah_trace_watch *watch, ta_definition_id definition);

/* Whether `id` is in the set: constant time, for tests and the scheduler. */
bool tah_id_set_has(const tah_id_set *set, uint64_t id);

/*
 * Marks `trace` watched when `actor` is: its session is watched, or its
 * definition's family is. A watched delivery with no trace starts one from
 * core's generator. `trace` is left alone otherwise.
 */
void tah_trace_watch_mark(tah_trace_watch *watch, ta_system *core, ta_actor_id actor, ta_trace_parent *trace);
