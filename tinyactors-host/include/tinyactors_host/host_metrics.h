#pragma once
/*
 * What only the host knows, for metrics (napi ADR 0017 section 7, core ADR
 * 0072 section 4): how long pump turns take, how late delayed sends fire,
 * faults by code, quarantines and dead letters by reason.
 *
 * Histograms have explicit buckets, as OTLP's Histogram does, because
 * otel-tui and most viewers chart those and not exponential ones. Each
 * bucket keeps its latest exemplar: the value, when it happened, and the
 * trace and span of a sampled macrostep that ran then, so a slow turn on a
 * dashboard leads to its trace.
 *
 * Everything here is fixed-size and allocates nothing. This module has no
 * Node-API in it.
 */
#include <stdbool.h>
#include <stdint.h>

#include "tinyactors.h"

/* Bounds of the largest histogram, plus one overflow bucket. */
#define TAH_HISTOGRAM_MAX_BOUNDS 16u

typedef struct {
    bool set;
    double value;       /* in the histogram's unit */
    uint64_t time_ns;   /* Unix nanoseconds */
    uint8_t trace_id[16];
    uint8_t span_id[8];
} tah_exemplar;

typedef struct {
    const double *bounds; /* ascending upper bounds, inclusive (OTLP explicit_bounds) */
    uint32_t bound_count;
    uint64_t count;
    double sum;
    double min;
    double max;
    uint64_t buckets[TAH_HISTOGRAM_MAX_BOUNDS + 1u];
    tah_exemplar exemplars[TAH_HISTOGRAM_MAX_BOUNDS + 1u];
} tah_histogram;

/* ta_result codes are small negative numbers; faults are counted by -code. */
#define TAH_FAULT_CODES 32u
/* The slot of the fault count for a runaway: an input that reached the microstep limit and was abandoned (core ADR
 * 0099 section 7). It is no ta_result, and it quarantines nothing. */
#define TAH_FAULT_RUNAWAY (TAH_FAULT_CODES - 1u)
/* tah_dead_reason, in scheduler.h. */
#define TAH_DEAD_REASONS 9u

typedef struct tah_host_metrics tah_host_metrics;

void tah_host_metrics_init(tah_host_metrics *metrics);

typedef enum {
    TAH_HISTOGRAM_TURN = 0, /* turn_seconds */
    TAH_HISTOGRAM_LATENESS, /* lateness_seconds */
    TAH_HISTOGRAM_LOAD      /* load_seconds */
} tah_histogram_kind;
/* One of the host's histograms, for a binding to report. */
const tah_histogram *tah_host_metrics_histogram(const tah_host_metrics *metrics, tah_histogram_kind kind);
/* An exemplar for the step `span_id` of `context`, set only when the
 * context is a sampled trace: an unsampled one has no trace to link to. */
tah_exemplar tah_exemplar_of(const ta_trace_context *context, uint64_t span_id, uint64_t time_ns);
/* Records one value; `exemplar` may be NULL or unset. */
void tah_histogram_record(tah_histogram *histogram, double value, const tah_exemplar *exemplar);
