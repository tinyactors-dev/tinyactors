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
    uint32_t max_subscriptions;
} bench_host_config;

typedef struct bench_host_cbor {
    ta_cbor_token tokens[256];
    char strings[4096];
    unsigned char bytes[8192];
    size_t length;
    unsigned char error[512]; /* the error reply of a refused config */
    ta_buffer error_reply;
} bench_host_cbor;

static void bench_host_number(ta_cbor_writer *w, const char *name, uint64_t value, uint32_t *pairs) {
    if (!value) return;
    ta_cbor_write_text(w, ta_string(name));
    ta_cbor_write_uint(w, value);
    (*pairs)++;
}

/* The config's CBOR into out->bytes; 0 if it does not fit. */
BENCH_UNUSED static int bench_host_config_cbor(const bench_host_config *c, bench_host_cbor *out) {
    for (int pass = 0; pass < 2; pass++) { /* count the pairs, then write them */
        static uint32_t top, system, telemetry;
        uint32_t counted_top = 0, counted_system = 0, counted_telemetry = 0;
        ta_cbor_writer writer, *w = &writer;
        memset(w, 0, sizeof *w);
        w->document.tokens = out->tokens;
        w->document.capacity = 256;
        w->strings = (ta_buffer){out->strings, sizeof out->strings, 0};
        const ta_system_config *s = &c->system;
        ta_cbor_write_map(w, pass ? top : 0);
        ta_cbor_write_text(w, (ta_string_view){"systemID", 8});
        ta_cbor_write_text(w, c->system_id);
        counted_top++;
        if (pass == 0 || system) {
            ta_cbor_write_text(w, ta_string("system"));
            ta_cbor_write_map(w, pass ? system : 0);
            bench_host_number(w, "maxActors", s->max_actors, &counted_system);
            bench_host_number(w, "maxEvents", s->max_events, &counted_system);
            bench_host_number(w, "maxTimers", s->max_timers, &counted_system);
            bench_host_number(w, "workspaces", s->workspaces, &counted_system);
            bench_host_number(w, "maxRequests", s->max_requests, &counted_system);
            bench_host_number(w, "outbox", s->outbox, &counted_system);
            bench_host_number(w, "quantum", s->quantum, &counted_system);
            bench_host_number(w, "instanceBytes", s->instance_bytes, &counted_system);
            bench_host_number(w, "stringBytes", s->string_bytes, &counted_system);
            bench_host_number(w, "ticksPerSecond", s->ticks_per_second, &counted_system);
            bench_host_number(w, "traceBytes", s->trace_bytes, &counted_system);
            bench_host_number(w, "traceContexts", s->trace_contexts, &counted_system);
            bench_host_number(w, "traceRules", s->trace_rules, &counted_system);
            bench_host_number(w, "traceSeed", s->trace_seed, &counted_system);
            if (s->tracing == TA_TRACING_DISABLED) {
                ta_cbor_write_text(w, ta_string("tracing"));
                ta_cbor_write_simple(w, TA_CBOR_FALSE);
                counted_system++;
            }
            if (pass == 0 && counted_system) counted_top++;
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
            counted_top++;
        }
        bench_host_number(w, "migrateQuantum", c->migrate_quantum, &counted_top);
        bench_host_number(w, "maxSubscriptions", c->max_subscriptions, &counted_top);
        if (c->telemetry.otlp.size) {
            const ta_otlp_config *o = &c->telemetry.otlp;
            ta_cbor_write_text(w, ta_string("telemetry"));
            ta_cbor_write_map(w, pass ? telemetry : 0);
            bench_host_number(w, "stagingBytes", o->staging_bytes, &counted_telemetry);
            bench_host_number(w, "logBytes", o->log_bytes, &counted_telemetry);
            bench_host_number(w, "openSpans", o->open_spans, &counted_telemetry);
            bench_host_number(w, "chunks", o->chunks, &counted_telemetry);
            bench_host_number(w, "scopes", o->scopes, &counted_telemetry);
            bench_host_number(w, "valueLimit", o->value_limit, &counted_telemetry);
            bench_host_number(w, "renderBytes", o->render_bytes, &counted_telemetry);
            bench_host_number(w, "resourceBytes", o->resource_bytes, &counted_telemetry);
            bench_host_number(w, "metricBytes", o->metric_bytes, &counted_telemetry);
            bench_host_number(w, "spanMetricCharts", o->span_metric_charts, &counted_telemetry);
            bench_host_number(w, "flushInterval", c->telemetry.flush_interval, &counted_telemetry);
            const char *flags[3] = {"linkParents", "values", "logs"};
            int set[3] = {(o->flags & TA_OTLP_LINK_PARENTS) != 0, (o->flags & TA_OTLP_VALUES) != 0,
                          (o->flags & TA_OTLP_NO_LOGS) == 0};
            int fallback[3] = {0, 0, 1};
            for (int k = 0; k < 3; k++)
                if (set[k] != fallback[k]) {
                    ta_cbor_write_text(w, ta_string(flags[k]));
                    ta_cbor_write_simple(w, set[k] ? TA_CBOR_TRUE : TA_CBOR_FALSE);
                    counted_telemetry++;
                }
            if (c->telemetry.resource_count) {
                ta_cbor_write_text(w, ta_string("resource"));
                ta_cbor_write_map(w, c->telemetry.resource_count);
                for (uint32_t i = 0; i < c->telemetry.resource_count; i++) {
                    ta_cbor_write_text(w, c->telemetry.resource[i].key);
                    ta_cbor_write_text(w, c->telemetry.resource[i].value);
                }
                counted_telemetry++;
            }
            counted_top++;
        }
        if (pass == 0) {
            top = counted_top;
            system = counted_system;
            telemetry = counted_telemetry;
            continue;
        }
        ta_buffer bytes = {out->bytes, sizeof out->bytes, 0};
        if (ta_cbor_finish(w, &bytes) != TA_OK) return 0;
        out->length = bytes.length;
    }
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
