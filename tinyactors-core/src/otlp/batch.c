/* batch.c — OTLP/HTTP bodies gathered per signal for an exporter
 * (include/tinyactors/otlp.h, "exporting"). Freestanding: it allocates
 * through the allocator it was given. */
#include <tinyactors/otlp.h>

#include "base/memory.h"

typedef struct batch_signal {
    unsigned char *bytes; /* waiting, then (in_flight) the body being posted ... */
    size_t length, capacity;
    size_t posting; /* ... its first `posting` bytes */
    ta_time since;  /* when the first waiting byte came */
    int in_flight;
} batch_signal;

struct ta_otlp_batch {
    ta_allocator allocator;
    batch_signal signals[TA_OTLP_SIGNALS];
    uint64_t posted, failed, dropped, failures, reported;
    ta_time last_report;
    int ever_reported;
};

ta_status ta_otlp_batch_new(const ta_allocator *allocator, ta_otlp_batch **out) {
    *out = NULL;
    if (!allocator || !allocator->alloc || !allocator->free) return TA_ERROR_INVALID;
    ta_otlp_batch *batch = (ta_otlp_batch *)allocator->alloc(allocator->context, sizeof *batch, 8);
    if (!batch) return TA_ERROR_NO_MEMORY;
    ta_memset(batch, 0, sizeof *batch);
    batch->allocator = *allocator;
    *out = batch;
    return TA_OK;
}

void ta_otlp_batch_free(ta_otlp_batch *batch) {
    if (!batch) return;
    ta_allocator allocator = batch->allocator;
    for (uint32_t i = 0; i < TA_OTLP_SIGNALS; i++)
        if (batch->signals[i].bytes)
            allocator.free(allocator.context, batch->signals[i].bytes, batch->signals[i].capacity);
    allocator.free(allocator.context, batch, sizeof *batch);
}

ta_status ta_otlp_batch_add(ta_otlp_batch *batch, uint32_t signal, const void *bytes, size_t length,
                            ta_time now) {
    if (signal >= TA_OTLP_SIGNALS) return TA_ERROR_INVALID;
    if (!length) return TA_OK;
    batch_signal *s = &batch->signals[signal];
    size_t waiting = s->length - s->posting;
    if (waiting + length > TA_OTLP_BATCH_MAX) {
        batch->dropped += length;
        batch->failures++;
        return TA_ERROR_NO_MEMORY;
    }
    if (s->length + length > s->capacity) {
        size_t capacity = s->capacity ? s->capacity : 4096;
        while (capacity < s->length + length)
            capacity *= 2;
        unsigned char *grown =
            (unsigned char *)batch->allocator.alloc(batch->allocator.context, capacity, 8);
        if (!grown) {
            batch->dropped += length;
            batch->failures++;
            return TA_ERROR_NO_MEMORY;
        }
        if (s->bytes) {
            ta_memcpy(grown, s->bytes, s->length);
            batch->allocator.free(batch->allocator.context, s->bytes, s->capacity);
        }
        s->bytes = grown;
        s->capacity = capacity;
    }
    if (s->length == s->posting) s->since = now;
    ta_memcpy(s->bytes + s->length, bytes, length);
    s->length += length;
    return TA_OK;
}

static int batch_due(const batch_signal *s, ta_time now, int flush) {
    size_t waiting = s->length - s->posting;
    return !s->in_flight && waiting &&
           (flush || waiting >= TA_OTLP_BATCH_BYTES || now >= s->since + TA_OTLP_BATCH_DELAY);
}

int ta_otlp_batch_next(ta_otlp_batch *batch, ta_time now, int flush, uint32_t *signal,
                       ta_string_view *body) {
    for (uint32_t i = 0; i < TA_OTLP_SIGNALS; i++) {
        batch_signal *s = &batch->signals[i];
        if (!batch_due(s, now, flush)) continue;
        s->in_flight = 1;
        s->posting = s->length;
        *signal = i;
        *body = (ta_string_view){(const char *)s->bytes, (uint32_t)s->length};
        return 1;
    }
    return 0;
}

void ta_otlp_batch_done(ta_otlp_batch *batch, uint32_t signal, int posted, ta_time now) {
    if (signal >= TA_OTLP_SIGNALS || !batch->signals[signal].in_flight) return;
    batch_signal *s = &batch->signals[signal];
    if (posted)
        batch->posted += s->posting;
    else {
        batch->failed += s->posting;
        batch->failures++;
    }
    /* what came while it was in flight moves to the front */
    if (s->length > s->posting) ta_memmove(s->bytes, s->bytes + s->posting, s->length - s->posting);
    s->length -= s->posting;
    s->posting = 0;
    s->in_flight = 0;
    (void)now; /* what waits keeps the time its first byte came */
}

ta_time ta_otlp_batch_deadline(const ta_otlp_batch *batch) {
    ta_time next = UINT64_MAX;
    for (uint32_t i = 0; i < TA_OTLP_SIGNALS; i++) {
        const batch_signal *s = &batch->signals[i];
        if (s->in_flight || s->length == s->posting) continue;
        ta_time due =
            s->length - s->posting >= TA_OTLP_BATCH_BYTES ? 0 : s->since + TA_OTLP_BATCH_DELAY;
        if (due < next) next = due;
    }
    return next;
}

int ta_otlp_batch_report(ta_otlp_batch *batch, ta_time now, uint64_t *failures) {
    *failures = batch->failures;
    if (batch->failures == batch->reported) return 0;
    if (batch->ever_reported && now < batch->last_report + TA_OTLP_BATCH_REPORT) return 0;
    batch->reported = batch->failures;
    batch->last_report = now;
    batch->ever_reported = 1;
    return 1;
}

void ta_otlp_batch_stats_get(const ta_otlp_batch *batch, ta_otlp_batch_stats *out) {
    ta_otlp_batch_stats stats;
    ta_memset(&stats, 0, sizeof stats);
    stats.size = sizeof stats;
    for (uint32_t i = 0; i < TA_OTLP_SIGNALS; i++) {
        stats.in_flight += (uint32_t)batch->signals[i].in_flight;
        stats.waiting += batch->signals[i].length - batch->signals[i].posting;
    }
    stats.posted = batch->posted;
    stats.failed = batch->failed;
    stats.dropped = batch->dropped;
    stats.failures = batch->failures;
    uint32_t n = out->size && out->size < sizeof stats ? out->size : (uint32_t)sizeof stats;
    ta_memcpy(out, &stats, n);
    out->size = n;
}
