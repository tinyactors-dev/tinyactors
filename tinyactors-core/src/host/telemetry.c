/* The host's OTLP encoder (docs/adr/0009 in the repository root, decision 6):
 * every segment's trace records go into one ta_otlp inside ta_host_run, and
 * an effect says when a request is ready for ta_host_traces or ta_host_logs.
 * Without an encoder (ta_host_telemetry.otlp.size 0) none of this runs. */
#include "base/memory.h"
#include "host/host_internal.h"

static void encoder_capacities(ta_otlp_config *config) {
    if (!config->open_spans) config->open_spans = 1024;
    if (!config->chunks) config->chunks = 8u * config->open_spans;
    if (!config->scopes) config->scopes = 64;
}

ta_status host_telemetry_init(ta_host *host) {
    const ta_host_telemetry *telemetry = &host->config.telemetry;
    if (!telemetry->otlp.size) return TA_OK;
    ta_otlp_config config;
    ta_memset(&config, 0, sizeof config);
    ta_memcpy(&config, &telemetry->otlp,
              telemetry->otlp.size < sizeof config ? telemetry->otlp.size : sizeof config);
    config.size = sizeof config;
    encoder_capacities(&config);
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
    return TA_OK;
}

void host_telemetry_free(ta_host *host) {
    host_free(host, host->otlp_memory, host->otlp_bytes);
    host->otlp = NULL;
    host->otlp_memory = NULL;
}

void host_telemetry_mark(ta_host *host, uint64_t wall_ns) {
    if (host->otlp) ta_otlp_mark(host->otlp, host->fuel_clock, wall_ns);
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
void host_telemetry_drain(ta_host *host) {
    if (!host->otlp) return;
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
            while ((status = ta_otlp_read(host->otlp, &input, &cursor)) == TA_OTLP_GROW)
                if (!encoder_grow(host)) break;
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
void host_telemetry_effects(ta_host *host) {
    if (!host->otlp) return;
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
void host_telemetry_note(ta_host *host) {
    if (!host->otlp || host->staged) return;
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
ta_time host_telemetry_deadline(const ta_host *host) {
    ta_time interval = host->config.telemetry.flush_interval;
    if (!host->otlp || !host->staged || !interval) return UINT64_MAX;
    if (host->traces_pending && host->logs_pending) return UINT64_MAX;
    return host->staged_since + interval;
}

static ta_status take(ta_host *host, uint32_t kind, ta_buffer *out) {
    if (host->running || !host->otlp) return TA_ERROR_STATE;
    host_telemetry_drain(host);
    ta_status status = kind == TA_HOST_EFFECT_TRACES ? ta_otlp_put_traces(host->otlp, out)
                                                     : ta_otlp_put_logs(host->otlp, out);
    if (status != TA_OK) return status;
    if (kind == TA_HOST_EFFECT_TRACES)
        host->traces_pending = 0;
    else
        host->logs_pending = 0;
    host->staging_full = 0;
    if (!staged(host, TA_HOST_EFFECT_TRACES) && !staged(host, TA_HOST_EFFECT_LOGS))
        host->staged = 0;
    return TA_OK;
}

ta_status ta_host_traces(ta_host *host, ta_buffer *out) {
    return take(host, TA_HOST_EFFECT_TRACES, out);
}

ta_status ta_host_logs(ta_host *host, ta_buffer *out) {
    return take(host, TA_HOST_EFFECT_LOGS, out);
}
