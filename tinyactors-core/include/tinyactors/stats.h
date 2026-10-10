/* tinyactors/stats.h — what a system and a host count, as plain structs
 * (docs/adr/0007 in the repository root).
 *
 * Only types: the functions that fill them are in tinyactors/system.h and
 * tinyactors/host.h. The structs are here so that a reader that must not
 * depend on the system layer (the otlp codec, tinyactors/otlp.h) can take
 * them. Every struct is size-prefixed: set `size` first, and exactly that
 * much is written. Counters only grow; gauges are the value now.
 */
#ifndef TINYACTORS_STATS_H
#define TINYACTORS_STATS_H

#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pool occupancy and monotonic counters of one system, O(1). */
typedef struct ta_system_stats {
    uint32_t size;
    uint32_t actors, actors_capacity;
    uint32_t runnable; /* actors in the run queue */
    uint32_t events, events_capacity;
    uint32_t timers, timers_capacity;
    uint32_t workspaces, workspaces_capacity;
    uint32_t requests, requests_capacity;
    uint32_t slab_pages, slab_pages_capacity, slab_page_bytes; /* instances + strings */
    uint32_t outbox, outbox_capacity;
    uint32_t pad;
    uint64_t region_bytes;
    uint64_t delivered;    /* events placed in mailboxes */
    uint64_t fuel;         /* work units consumed by ta_system_run */
    uint64_t turns;        /* actor scheduling turns */
    uint64_t timers_fired; /* delayed sends that came due */
    uint64_t spawned;      /* sessions created (last session id) */
    ta_time now;
    uint32_t hibernated; /* actors currently unloaded */
    uint32_t wakes_pending;
    uint64_t hibernations, wakes; /* monotonic */
    /* tracing: the journal (bytes waiting, its capacity, records written and
     * lost), the context pool, and contexts not given for a full pool */
    uint32_t trace_bytes, trace_bytes_capacity;
    uint32_t trace_contexts, trace_contexts_capacity;
    uint64_t trace_records, trace_records_lost, trace_contexts_dropped;
    uint64_t fuel_clock; /* the fuel clock records are stamped with (ta_system_trace_clock) */
    /* traces this system started (a host input, a spawn, a timer of an
     * untraced send; with or without a caller's traceparent), and how many
     * of them were sampled */
    uint64_t traces_started, traces_sampled;
} ta_system_stats;

/* What one chart's actors did in one system (ta_system_chart_stats_get), or
 * in every segment of a host that has a chart of that name, retired ones
 * included (ta_host_chart_stats). Counted once per scheduling turn and on
 * the paths that fail or change an actor's lifecycle, never per event. */
typedef struct ta_chart_stats {
    uint32_t size;
    uint32_t chart;      /* its index in the system's image (a host: in the current version's) */
    ta_string_view name; /* the chart's name; valid until the next mutating call */
    uint32_t actors;     /* gauges: live actors of the chart, resident or hibernated */
    uint32_t hibernated; /* ... of which hibernated */
    uint64_t spawned;    /* sessions spawned (not adopted) */
    uint64_t ended;      /* sessions that finished, were terminated or faulted (not exported) */
    uint64_t turns;      /* scheduling turns of its actors */
    uint64_t macrosteps; /* macrosteps its actors began, the bootstrap included */
    uint64_t fuel;       /* fuel its actors used */
    /* platform errors raised in its actors: error.execution, error.communication
     * (an event for a session that is gone among them), and error.platform.* */
    uint64_t errors_execution, errors_communication, errors_platform;
    uint64_t faults; /* actors reclaimed after a fault (TA_EFFECT_ERROR) */
} ta_chart_stats;

/* The host's own counters. */
typedef struct ta_host_stats {
    uint32_t size;
    uint32_t segments; /* live */
    uint32_t actors, hibernated, runnable, jobs;
    uint64_t region_bytes, delivered, routed, migrated, retired;
    /* the fuel clock every segment's trace records are stamped with: note it,
     * with the wall clock, before and after each ta_host_run (tinyactors/otlp.h) */
    uint64_t fuel_clock;
} ta_host_stats;

#ifdef __cplusplus
}
#endif

#endif /* TINYACTORS_STATS_H */
