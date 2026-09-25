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

typedef struct {
    ta_allocator allocator;
    uint64_t *ids;           /* sessions, then definitions */
    uint32_t session_count;
    uint32_t definition_count;
} tna_trace_watch;

void tna_trace_watch_init(tna_trace_watch *watch, ta_allocator allocator);
void tna_trace_watch_destroy(tna_trace_watch *watch);

typedef struct {
    const uint64_t *sessions;
    uint32_t session_count;
    const ta_definition_id *definitions;
    uint32_t definition_count;
} tna_trace_watch_set_params;

/* Replaces what is watched. TA_ERR_NOMEM leaves the watch as it was. */
ta_result tna_trace_watch_set(tna_trace_watch *watch, const tna_trace_watch_set_params *params);

static inline bool tna_trace_watch_empty(const tna_trace_watch *watch) {
    return watch == NULL || watch->session_count + watch->definition_count == 0;
}

/*
 * Marks `trace` watched when `actor` is: its session or its definition is
 * watched. A watched delivery with no trace starts one from core's
 * generator. `trace` is left alone otherwise.
 */
void tna_trace_watch_mark(const tna_trace_watch *watch, ta_system *core, ta_actor_id actor, ta_trace_parent *trace);
