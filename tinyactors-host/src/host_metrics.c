#include "host_metrics.h"

/* Seconds. A pump turn is normally well under a millisecond; a turn that
 * takes a second means the event loop was blocked. */
static const double tah_turn_bounds[] = {
    0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0,
};

/* Seconds. A timer that fires a few milliseconds late is normal; seconds
 * late means the host is not keeping up. */
static const double tah_lateness_bounds[] = {
    0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0,
};

/* Seconds. A loader reads storage: milliseconds are normal; seconds mean
 * the store is slow, and the default timeout is five. */
static const double tah_load_bounds[] = {
    0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0,
};

static void tah_histogram_init(tah_histogram *histogram, const double *bounds, uint32_t count) {
    *histogram = (tah_histogram){ .bounds = bounds, .bound_count = count };
}

void tah_host_metrics_init(tah_host_metrics *metrics) {
    *metrics = (tah_host_metrics){ 0 };
    tah_histogram_init(&metrics->turn_seconds, tah_turn_bounds,
                       (uint32_t)(sizeof tah_turn_bounds / sizeof tah_turn_bounds[0]));
    tah_histogram_init(&metrics->lateness_seconds, tah_lateness_bounds,
                       (uint32_t)(sizeof tah_lateness_bounds / sizeof tah_lateness_bounds[0]));
    tah_histogram_init(&metrics->load_seconds, tah_load_bounds,
                       (uint32_t)(sizeof tah_load_bounds / sizeof tah_load_bounds[0]));
}

tah_exemplar tah_exemplar_of(const ta_trace_context *context, uint64_t span_id, uint64_t time_ns) {
    tah_exemplar exemplar = { 0 };
    if ((context->flags & TA_TRACE_FLAG_SAMPLED) == 0 || (context->trace_id_high | context->trace_id_low) == 0) {
        return exemplar;
    }
    exemplar.set = true;
    exemplar.time_ns = time_ns;
    for (int at = 0; at < 8; at += 1) {
        exemplar.trace_id[at] = (uint8_t)(context->trace_id_high >> (56 - 8 * at));
        exemplar.trace_id[8 + at] = (uint8_t)(context->trace_id_low >> (56 - 8 * at));
        exemplar.span_id[at] = (uint8_t)(span_id >> (56 - 8 * at));
    }
    return exemplar;
}

void tah_histogram_record(tah_histogram *histogram, double value, const tah_exemplar *exemplar) {
    uint32_t bucket = 0;
    while (bucket < histogram->bound_count && value > histogram->bounds[bucket]) {
        bucket += 1u;
    }
    if (histogram->count == 0 || value < histogram->min) {
        histogram->min = value;
    }
    if (histogram->count == 0 || value > histogram->max) {
        histogram->max = value;
    }
    histogram->count += 1u;
    histogram->sum += value;
    histogram->buckets[bucket] += 1u;
    if (exemplar != NULL && exemplar->set) {
        histogram->exemplars[bucket] = *exemplar;
        histogram->exemplars[bucket].value = value;
    }
}
