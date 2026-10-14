/* bench_host_config.h — the torture bench describes a host as a C struct;
 * this writes it as the CBOR config of ta_host_create (docs/host-config.md)
 * with the CBOR writer, and fills the environment. Only what is set
 * (non-zero) is written, so the bench's defaults are the host's. The bench's
 * own copy: tests/support/host_config.h is the tests'. */
#ifndef TINYACTORS_TORTURE_BENCH_HOST_CONFIG_H
#define TINYACTORS_TORTURE_BENCH_HOST_CONFIG_H

#include <stdio.h>
#include <string.h>

#include <tinyactors/cbor.h>
#include <tinyactors/host.h>

#ifndef BENCH_UNUSED
#define BENCH_UNUSED __attribute__((unused))
#endif

typedef struct bench_host_telemetry {
    ta_otlp_config otlp; /* size 0: no "telemetry" */
    ta_time flush_interval;
    const ta_otlp_attribute *resource;
    uint32_t resource_count;
} bench_host_telemetry;

typedef struct bench_host_config {
    uint32_t size; /* not read: tests set it as they always did */
    uint32_t datamodel_count;
    ta_string_view system_id;
    ta_system_config system; /* capacities, tracing; trace_policy goes to the environment */
    const ta_datamodel_binding *datamodels;
    const ta_io_processor_descriptor *io_processors;
    uint32_t io_processor_count;
    uint32_t migrate_quantum;
    ta_allocator allocator;
    bench_host_telemetry telemetry;
} bench_host_config;

typedef struct bench_host_cbor {
    unsigned char bytes[8192];
    size_t length;
    unsigned char error[512]; /* the error reply of a refused config */
    ta_buffer error_reply;
} bench_host_cbor;

static void bench_host_number(ta_cbor_writer *w, const char *name, uint64_t value) {
    if (!value) return;
    ta_cbor_write_text(w, ta_string(name));
    ta_cbor_write_uint(w, value);
}

/* The config's CBOR into out->bytes; 0 if it does not fit. Maps whose
 * members are only those set are counted as they close. */
BENCH_UNUSED static int bench_host_config_cbor(const bench_host_config *c, bench_host_cbor *out) {
    ta_cbor_writer writer, *w = &writer;
    ta_cbor_writer_init(w, (ta_buffer){out->bytes, sizeof out->bytes, 0}, NULL);
    const ta_system_config *s = &c->system;
    ta_cbor_write_map(w, TA_CBOR_COUNT_LATER);
    ta_cbor_write_text(w, (ta_string_view){"systemID", 8});
    ta_cbor_write_text(w, c->system_id);
    int system = s->max_actors || s->max_events || s->max_timers || s->workspaces || s->max_requests ||
                 s->outbox || s->quantum || s->instance_bytes || s->string_bytes || s->ticks_per_second ||
                 s->trace_bytes || s->trace_contexts || s->trace_rules || s->trace_seed ||
                 s->tracing == TA_TRACING_DISABLED;
    if (system) {
        ta_cbor_write_text(w, ta_string("system"));
        ta_cbor_write_map(w, TA_CBOR_COUNT_LATER);
        bench_host_number(w, "maxActors", s->max_actors);
        bench_host_number(w, "maxEvents", s->max_events);
        bench_host_number(w, "maxTimers", s->max_timers);
        bench_host_number(w, "workspaces", s->workspaces);
        bench_host_number(w, "maxRequests", s->max_requests);
        bench_host_number(w, "outbox", s->outbox);
        bench_host_number(w, "quantum", s->quantum);
        bench_host_number(w, "instanceBytes", s->instance_bytes);
        bench_host_number(w, "stringBytes", s->string_bytes);
        bench_host_number(w, "ticksPerSecond", s->ticks_per_second);
        bench_host_number(w, "traceBytes", s->trace_bytes);
        bench_host_number(w, "traceContexts", s->trace_contexts);
        bench_host_number(w, "traceRules", s->trace_rules);
        bench_host_number(w, "traceSeed", s->trace_seed);
        if (s->tracing == TA_TRACING_DISABLED) {
            ta_cbor_write_text(w, ta_string("tracing"));
            ta_cbor_write_simple(w, TA_CBOR_FALSE);
        }
        ta_cbor_write_end(w);
    }
    if (c->io_processor_count) {
        ta_cbor_write_text(w, ta_string("ioProcessors"));
        ta_cbor_write_array(w, c->io_processor_count);
        for (uint32_t i = 0; i < c->io_processor_count; i++) {
            const ta_io_processor_descriptor *d = &c->io_processors[i];
            ta_cbor_write_map(w, 3);
            ta_cbor_write_text(w, ta_string("type"));
            ta_cbor_write_text(w, d->type);
            ta_cbor_write_text(w, ta_string("location"));
            ta_cbor_write_text(w, d->location);
            ta_cbor_write_text(w, ta_string("sync"));
            ta_cbor_write_simple(w, d->flags & TA_IO_PROCESSOR_SYNC ? TA_CBOR_TRUE : TA_CBOR_FALSE);
        }
    }
    bench_host_number(w, "migrateQuantum", c->migrate_quantum);
    if (c->telemetry.otlp.size) {
        const ta_otlp_config *o = &c->telemetry.otlp;
        ta_cbor_write_text(w, ta_string("telemetry"));
        ta_cbor_write_map(w, TA_CBOR_COUNT_LATER);
        bench_host_number(w, "stagingBytes", o->staging_bytes);
        bench_host_number(w, "logBytes", o->log_bytes);
        bench_host_number(w, "openSpans", o->open_spans);
        bench_host_number(w, "chunks", o->chunks);
        bench_host_number(w, "scopes", o->scopes);
        bench_host_number(w, "valueLimit", o->value_limit);
        bench_host_number(w, "renderBytes", o->render_bytes);
        bench_host_number(w, "resourceBytes", o->resource_bytes);
        bench_host_number(w, "metricBytes", o->metric_bytes);
        bench_host_number(w, "spanMetricCharts", o->span_metric_charts);
        bench_host_number(w, "flushInterval", c->telemetry.flush_interval);
        const char *flags[3] = {"linkParents", "values", "logs"};
        int set[3] = {(o->flags & TA_OTLP_LINK_PARENTS) != 0, (o->flags & TA_OTLP_VALUES) != 0,
                      (o->flags & TA_OTLP_NO_LOGS) == 0};
        int fallback[3] = {0, 0, 1};
        for (int k = 0; k < 3; k++)
            if (set[k] != fallback[k]) {
                ta_cbor_write_text(w, ta_string(flags[k]));
                ta_cbor_write_simple(w, set[k] ? TA_CBOR_TRUE : TA_CBOR_FALSE);
            }
        if (c->telemetry.resource_count) {
            ta_cbor_write_text(w, ta_string("resource"));
            ta_cbor_write_map(w, c->telemetry.resource_count);
            for (uint32_t i = 0; i < c->telemetry.resource_count; i++) {
                ta_cbor_write_text(w, c->telemetry.resource[i].key);
                ta_cbor_write_text(w, c->telemetry.resource[i].value);
            }
        }
        ta_cbor_write_end(w);
    }
    ta_cbor_write_end(w);
    ta_bytes bytes;
    if (ta_cbor_writer_bytes(w, &bytes) != TA_OK) return 0; /* (no allocator: nothing to free) */
    out->length = bytes.length;
    return 1;
}

BENCH_UNUSED static ta_host_environment bench_host_environment(const bench_host_config *c,
                                                             bench_host_cbor *cbor) {
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodels = c->datamodels;
    environment.datamodel_count = c->datamodel_count;
    environment.allocator = c->allocator;
    environment.trace_policy = c->system.trace_policy;
    cbor->error_reply = (ta_buffer){cbor->error, sizeof cbor->error, 0};
    environment.error = &cbor->error_reply;
    return environment;
}

/* ta_host_create with `c` as its config and environment. */
BENCH_UNUSED static ta_status bench_host_new(const bench_host_config *c, ta_host **out) {
    static bench_host_cbor cbor;
    *out = NULL;
    if (!bench_host_config_cbor(c, &cbor)) return TA_ERROR_NO_MEMORY;
    ta_host_environment environment = bench_host_environment(c, &cbor);
    return ta_host_create((ta_bytes){cbor.bytes, cbor.length}, &environment, out);
}

/* ta_host_restore with `c` as its config and environment. */
BENCH_UNUSED static ta_status bench_host_restore(const bench_host_config *c,
                                               const ta_host_snapshot_input *input, ta_host **out) {
    static bench_host_cbor cbor;
    *out = NULL;
    if (!bench_host_config_cbor(c, &cbor)) return TA_ERROR_NO_MEMORY;
    ta_host_environment environment = bench_host_environment(c, &cbor);
    ta_host_snapshot_input with_config = *input;
    with_config.config = (ta_bytes){cbor.bytes, cbor.length};
    return ta_host_restore(&environment, &with_config, out);
}

#endif /* TINYACTORS_TORTURE_BENCH_HOST_CONFIG_H */
