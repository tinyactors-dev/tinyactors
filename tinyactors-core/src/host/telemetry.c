/* The host's OTLP encoder (docs/adr/0009 in the repository root, decision 6):
 * every segment's trace records go into one ta_otlp inside ta_host_run, and
 * an effect says when a request is ready for ta_host_traces or ta_host_logs.
 * Without an encoder (host_telemetry.otlp.size 0: the config has no "telemetry") the host gets the null
 * encoder: every operation does nothing, as datamodel_null.c does for charts.
 * The choice is made once, at ta_host_create; nothing below asks again. */
#include "base/memory.h"
#include "host/host_internal.h"

static const host_encoder encoder_null, encoder_otlp;

ta_status host_telemetry_init(ta_host *host) {
    const host_telemetry *telemetry = &host->config.telemetry;
    host->encoder = &encoder_null;
    if (!telemetry->otlp.size) return TA_OK;
    ta_otlp_config config = telemetry->otlp; /* defaults filled in (host_new) */
    size_t bytes = 0;
    ta_status status = ta_otlp_layout(&config, &bytes);
    if (status != TA_OK) return status;
    void *memory = host_alloc(host, bytes);
    if (!memory) return TA_ERROR_NO_MEMORY;
    status = ta_otlp_init((ta_memory){memory, bytes}, &config, &host->otlp);
    if (status == TA_OK && telemetry->resource_count)
        status = ta_otlp_set_resource(host->otlp, telemetry->resource, telemetry->resource_count);
    if (status != TA_OK) {
        host_free(host, memory, bytes);
        host->otlp = NULL;
        return status;
    }
    host->otlp_config = config;
    host->otlp_memory = memory;
    host->otlp_bytes = bytes;
    host->encoder = &encoder_otlp;
    return TA_OK;
}

/* Spans still open are ended before the encoder goes, and dropped with it:
 * a caller who wants them sends trace.end and takes the traces first. */
void host_telemetry_free(ta_host *host) {
    if (host->otlp) ta_otlp_end_all(host->otlp, host->wall_ns);
    host_free(host, host->otlp_memory, host->otlp_bytes);
    host->otlp = NULL;
    host->otlp_memory = NULL;
}

static void otlp_mark(ta_host *host, uint64_t wall_ns) {
    ta_otlp_mark(host->otlp, host->fuel_clock, wall_ns);
    if (!host->wall_start_ns) host->wall_start_ns = wall_ns;
    host->wall_ns = wall_ns;
}

/* Open spans, chunks or scopes are full: twice as many in a new block. */
static int encoder_grow(ta_host *host) {
    ta_otlp_config config = host->otlp_config;
    config.open_spans *= 2;
    config.chunks *= 2;
    config.scopes *= 2;
    size_t bytes = 0;
    if (ta_otlp_layout(&config, &bytes) != TA_OK) return 0;
    void *memory = host_alloc(host, bytes);
    if (!memory) return 0;
    if (ta_otlp_relocate(&host->otlp, (ta_memory){memory, bytes}, &config) != TA_OK) {
        host_free(host, memory, bytes);
        return 0;
    }
    host_free(host, host->otlp_memory, host->otlp_bytes);
    host->otlp_config = config;
    host->otlp_memory = memory;
    host->otlp_bytes = bytes;
    return 1;
}

/* Read every segment's records into the encoder. It stops where the staging
 * is full (a request must be taken first) or the encoder cannot grow; the
 * records left stay in their journals. */
static void otlp_drain(ta_host *host) {
    ta_otlp_input input;
    ta_memset(&input, 0, sizeof input);
    input.size = sizeof input;
    input.datamodels = host->datamodels;
    input.datamodel_count = host->config.datamodel_count;
    for (uint32_t si = 0; si < host->segment_count; si++) {
        host_segment *segment = &host->segments[si];
        if (!segment->live) continue;
        input.source = si;
        input.image = segment->image;
        while (ta_system_trace(segment->system, &input.view)) {
            uint32_t cursor = 0, status;
            for (;;) {
                uint32_t was = host_guard(host); /* to_wire, for values */
                status = ta_otlp_read(host->otlp, &input, &cursor);
                host_unguard(host, was);
                if (status != TA_OTLP_GROW || !encoder_grow(host)) break;
            }
            if (status != TA_OTLP_READ) {
                if (status == TA_OTLP_FLUSH) host->staging_full = 1;
                ta_system_trace_ack(segment->system, cursor);
                return;
            }
            ta_system_trace_ack(segment->system, input.view.length);
        }
    }
}

static int staged(const ta_host *host, uint32_t kind) {
    ta_otlp_stats stats;
    stats.size = sizeof stats;
    ta_otlp_stats_get(host->otlp, &stats);
    return kind == TA_HOST_EFFECT_TRACES ? stats.spans != 0 : stats.log_records != 0;
}

/* After a run's drain: a ready request of each kind becomes an effect. */
static void otlp_effects(ta_host *host) {
    int spans = staged(host, TA_HOST_EFFECT_TRACES), logs = staged(host, TA_HOST_EFFECT_LOGS);
    if (!spans && !logs) {
        host->staging_full = 0;
        host->staged = 0;
        return;
    }
    if (!host->staged) {
        host->staged = 1;
        host->staged_since = host->now;
    }
    ta_time interval = host->config.telemetry.flush_interval;
    int due = host->staging_full || (interval && host->now - host->staged_since >= interval);
    if (!due) return;
    if (spans && !host->traces_pending && host_effect_new(host, TA_HOST_EFFECT_TRACES, TA_NIL))
        host->traces_pending = 1;
    if (logs && !host->logs_pending && host_effect_new(host, TA_HOST_EFFECT_LOGS, TA_NIL))
        host->logs_pending = 1;
}

/* After a run: records waiting in a journal count as staged from now, so
 * their flush interval is a deadline of the run (they are read by the next
 * run, after the ta_host_advance_time that stamps them). */
static void otlp_note(ta_host *host) {
    if (host->staged) return;
    for (uint32_t si = 0; si < host->segment_count; si++) {
        ta_trace_view view;
        if (host->segments[si].live && ta_system_trace(host->segments[si].system, &view)) {
            host->staged = 1;
            host->staged_since = host->now;
            return;
        }
    }
}

/* When the staged records' flush interval ends (UINT64_MAX: no deadline). */
static ta_time otlp_deadline(const ta_host *host) {
    ta_time interval = host->config.telemetry.flush_interval;
    if (!host->staged || !interval) return UINT64_MAX;
    if (host->traces_pending && host->logs_pending) return UINT64_MAX;
    return host->staged_since + interval;
}

/* End what fits of the open spans at the latest wall time, marked
 * scxml.incomplete; spans left open (the staging is full) are ended by the
 * takes that make room. */
static void otlp_end_open(ta_host *host) {
    ta_otlp_end_all(host->otlp, host->wall_ns);
    ta_otlp_stats stats;
    stats.size = sizeof stats;
    ta_otlp_stats_get(host->otlp, &stats);
    host->ending = stats.open != 0;
}

static ta_status otlp_take(ta_host *host, uint32_t kind, ta_buffer *out) {
    otlp_drain(host);
    ta_status status = kind == TA_HOST_EFFECT_TRACES ? ta_otlp_put_traces(host->otlp, out)
                                                     : ta_otlp_put_logs(host->otlp, out);
    if (status != TA_OK) return status;
    if (kind == TA_HOST_EFFECT_TRACES)
        host->traces_pending = 0;
    else
        host->logs_pending = 0;
    host->staging_full = 0;
    if (host->ending && kind == TA_HOST_EFFECT_TRACES) otlp_end_open(host);
    if (!staged(host, TA_HOST_EFFECT_TRACES) && !staged(host, TA_HOST_EFFECT_LOGS))
        host->staged = 0;
    return TA_OK;
}

/* The counters now as metrics (docs/adr/0007): per chart, per segment and
 * the host's own. Metrics staged by a call whose request did not fit are put
 * again as they are, not added twice. */
static ta_status otlp_metrics(ta_host *host, ta_buffer *out) {
    ta_otlp_stats encoder;
    encoder.size = sizeof encoder;
    ta_otlp_stats_get(host->otlp, &encoder);
    if (!encoder.metrics) {
        uint32_t live = 0, charts = host_chart_stats(host, NULL, 0);
        for (uint32_t si = 0; si < host->segment_count; si++)
            live += host->segments[si].live != 0;
        size_t bytes = sizeof(ta_system_stats) * live + sizeof(ta_chart_stats) * charts;
        unsigned char *block = bytes ? host_alloc(host, bytes) : NULL;
        if (bytes && !block) return TA_ERROR_NO_MEMORY;
        ta_system_stats *systems = (ta_system_stats *)(void *)block;
        ta_chart_stats *chart_stats =
            (ta_chart_stats *)(void *)(block + sizeof(ta_system_stats) * live);
        uint32_t n = 0;
        for (uint32_t si = 0; si < host->segment_count; si++)
            if (host->segments[si].live) {
                systems[n].size = sizeof systems[n];
                ta_system_stats_get(host->segments[si].system, &systems[n++]);
            }
        for (uint32_t c = 0; c < charts; c++)
            chart_stats[c].size = sizeof chart_stats[c];
        host_chart_stats(host, chart_stats, charts);
        ta_host_stats stats;
        stats.size = sizeof stats;
        host_stats_get(host, &stats);
        ta_otlp_stats_input input = {sizeof input, charts, chart_stats, systems, live, 0, &stats,
                                     host->config.system.ticks_per_second, host->wall_start_ns,
                                     host->wall_ns};
        ta_status status = ta_otlp_add_stats(host->otlp, &input);
        if (bytes) host_free(host, block, bytes);
        if (status != TA_OK) {
            out->length = 0;
            return status;
        }
    }
    return ta_otlp_put_metrics(host->otlp, out);
}

/* trace.end: the journals' records first, then every open span ends; they
 * are due at once, so a TRACES effect follows (outside a run, with the next
 * ta_host_effects). */
static ta_status otlp_end(ta_host *host) {
    otlp_drain(host);
    otlp_end_open(host);
    host->staging_full = 1; /* due now, whatever the flush interval */
    otlp_effects(host);
    return TA_OK;
}

static const host_encoder encoder_otlp = {otlp_mark,     otlp_drain, otlp_effects, otlp_note,
                                          otlp_deadline, otlp_take,  otlp_metrics, otlp_end,
                                          host_definitions_otlp};

/* ---- the null encoder: no encoder memory was given --------------------------- */
/* The journals stay with the caller, nothing is staged, no effect and no
 * deadline; a take is TA_ERROR_STATE (host.h). */
static void null_mark(ta_host *host, uint64_t wall_ns) { (void)host, (void)wall_ns; }
static void null_run(ta_host *host) { (void)host; }
static ta_time null_deadline(const ta_host *host) {
    (void)host;
    return UINT64_MAX;
}
static ta_status null_take(ta_host *host, uint32_t kind, ta_buffer *out) {
    (void)host, (void)kind;
    out->length = 0;
    return TA_ERROR_STATE;
}
static ta_status null_metrics(ta_host *host, ta_buffer *out) {
    (void)host;
    out->length = 0;
    return TA_ERROR_STATE;
}
static ta_status null_end(ta_host *host) {
    (void)host;
    return TA_ERROR_STATE;
}
static void null_definitions(ta_host *host, uint32_t kind, uint32_t segment) {
    (void)host, (void)kind, (void)segment;
}
static const host_encoder encoder_null = {null_mark, null_run,      null_run,  null_run,
                                          null_deadline, null_take, null_metrics, null_end,
                                          null_definitions};

ta_status ta_host_traces(ta_host *host, ta_buffer *out) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    return host->encoder->take(host, TA_HOST_EFFECT_TRACES, out);
}

ta_status ta_host_metrics(ta_host *host, ta_buffer *out) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    return host->encoder->metrics(host, out);
}

ta_status ta_host_logs(ta_host *host, ta_buffer *out) {
    if (host->running) return TA_ERROR_STATE; /* not during a run (host.h) */
    return host->encoder->take(host, TA_HOST_EFFECT_LOGS, out);
}
