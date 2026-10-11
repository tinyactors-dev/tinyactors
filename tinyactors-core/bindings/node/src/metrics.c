/* metrics.c — metrics: encoding, decoding and the host's counts (binding.h). */
#include "binding.h"

double named_double(napi_env environment, napi_value object, const char *name) {
    napi_value v;
    double d = 0;
    if (napi_get_named_property(environment, object, name, &v) == napi_ok)
        napi_get_value_double(environment, v, &d);
    return d;
}

uint32_t named_length(napi_env environment, napi_value object, const char *name,
                      napi_value *array) {
    bool is_array = false;
    uint32_t n = 0;
    if (napi_get_named_property(environment, object, name, array) == napi_ok)
        napi_is_array(environment, *array, &is_array);
    if (is_array) napi_get_array_length(environment, *array, &n);
    return n;
}

/* A metric of core.mjs onto `otlp`. */
static ta_status metric_add(napi_env environment, ta_otlp *otlp, napi_value metric,
                            uint64_t start_ns, uint64_t time_ns) {
    napi_value v, points_js;
    size_t name_length = 0, unit_length = 0;
    char *name = NULL, *unit = NULL;
    if (napi_get_named_property(environment, metric, "name", &v) == napi_ok)
        name = get_string(environment, v, &name_length);
    if (napi_get_named_property(environment, metric, "unit", &v) == napi_ok)
        unit = get_string(environment, v, &unit_length);
    uint32_t n = named_length(environment, metric, "points", &points_js);
    ta_otlp_point *points = (ta_otlp_point *)calloc(n ? n : 1, sizeof *points);
    js_attributes *attributes = (js_attributes *)calloc(n ? n : 1, sizeof *attributes);
    double **bounds = (double **)calloc(n ? n : 1, sizeof(double *));
    uint64_t **buckets = (uint64_t **)calloc(n ? n : 1, sizeof(uint64_t *));
    ta_otlp_exemplar **exemplars = (ta_otlp_exemplar **)calloc(n ? n : 1, sizeof(void *));
    for (uint32_t i = 0; i < n; i++) {
        napi_value point, list;
        napi_get_element(environment, points_js, i, &point);
        if (napi_get_named_property(environment, point, "attributes", &list) == napi_ok)
            attributes_of(environment, list, &attributes[i]);
        points[i].attributes = attributes[i].list;
        points[i].attribute_count = attributes[i].count;
        bool integer = false;
        if (napi_get_named_property(environment, point, "integer", &v) == napi_ok)
            napi_get_value_bool(environment, v, &integer);
        double value = named_double(environment, point, "value");
        if (integer)
            points[i].integer = (int64_t)value;
        else {
            points[i].flags = TA_OTLP_POINT_DOUBLE;
            points[i].number = value;
        }
        points[i].count = (uint64_t)named_double(environment, point, "count");
        points[i].sum = named_double(environment, point, "sum");
        points[i].min = named_double(environment, point, "min");
        points[i].max = named_double(environment, point, "max");
        uint32_t bucket_count = named_length(environment, point, "buckets", &list);
        buckets[i] = (uint64_t *)calloc(bucket_count ? bucket_count : 1, sizeof(uint64_t));
        for (uint32_t k = 0; k < bucket_count; k++) {
            napi_get_element(environment, list, k, &v);
            double d = 0;
            napi_get_value_double(environment, v, &d);
            buckets[i][k] = (uint64_t)d;
        }
        uint32_t bound_count = named_length(environment, point, "bounds", &list);
        bounds[i] = (double *)calloc(bound_count ? bound_count : 1, sizeof(double));
        for (uint32_t k = 0; k < bound_count; k++) {
            napi_get_element(environment, list, k, &v);
            napi_get_value_double(environment, v, &bounds[i][k]);
        }
        points[i].buckets = buckets[i];
        points[i].bounds = bounds[i];
        points[i].bucket_count = bucket_count && bound_count + 1 == bucket_count ? bucket_count : 0;
        uint32_t exemplar_count = named_length(environment, point, "exemplars", &list);
        exemplars[i] = (ta_otlp_exemplar *)calloc(exemplar_count ? exemplar_count : 1,
                                                  sizeof(ta_otlp_exemplar));
        uint32_t kept = 0;
        for (uint32_t k = 0; k < exemplar_count; k++) {
            napi_value exemplar;
            napi_get_element(environment, list, k, &exemplar);
            size_t tl = 0, sl = 0;
            char *trace = NULL, *span = NULL;
            if (napi_get_named_property(environment, exemplar, "traceID", &v) == napi_ok)
                trace = get_string(environment, v, &tl);
            if (napi_get_named_property(environment, exemplar, "spanID", &v) == napi_ok)
                span = get_string(environment, v, &sl);
            ta_otlp_exemplar *e = &exemplars[i][kept];
            uint64_t ignored;
            if (trace && span && tl == 32 && sl == 16 &&
                hex_text(trace, tl, &e->trace.high, &e->trace.low) &&
                hex_text(span, sl, &ignored, &e->span)) {
                e->time = (uint64_t)(named_double(environment, exemplar, "time") * 1e6);
                e->value = named_double(environment, exemplar, "value");
                kept++;
            }
            free(trace);
            free(span);
        }
        points[i].exemplars = exemplars[i];
        points[i].exemplar_count = kept;
    }
    ta_otlp_metric m;
    memset(&m, 0, sizeof m);
    m.name = (ta_string_view){name ? name : "", (uint32_t)name_length};
    m.unit = (ta_string_view){unit ? unit : "", (uint32_t)unit_length};
    m.kind = (uint32_t)named_double(environment, metric, "kind");
    m.points = points;
    m.point_count = n;
    m.start_ns = start_ns;
    m.time_ns = time_ns;
    ta_status status = ta_otlp_add_metric(otlp, &m);
    for (uint32_t i = 0; i < n; i++) {
        if (attributes[i].list) attributes_free(&attributes[i]);
        free(bounds[i]);
        free(buckets[i]);
        free(exemplars[i]);
    }
    free(points);
    free(attributes);
    free(bounds);
    free(buckets);
    free(exemplars);
    free(name);
    free(unit);
    return status;
}

/* An encoder for one metrics request, `metric_bytes` of staging. */
static ta_otlp *metrics_encoder(uint32_t metric_bytes, size_t *length) {
    ta_otlp_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.open_spans = 1;
    config.staging_bytes = TA_OTLP_STAGING_BYTES_MIN;
    config.log_bytes = 1024;
    config.scopes = 1;
    config.metric_bytes = metric_bytes;
    ta_otlp *otlp = NULL;
    if (ta_otlp_layout(&config, length) != TA_OK) return NULL;
    void *memory = malloc(*length);
    if (!memory || ta_otlp_init(memory, *length, &config, &otlp) != TA_OK) {
        free(memory);
        return NULL;
    }
    return otlp;
}

/* Twice the room; NULL (and the old one freed) when refused. */
static ta_otlp *metrics_grow(ta_otlp *otlp, size_t *length) {
    ta_otlp_stats stats;
    stats.size = sizeof stats;
    ta_otlp_stats_get(otlp, &stats);
    ta_otlp_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.open_spans = 1;
    config.staging_bytes = TA_OTLP_STAGING_BYTES_MIN;
    config.log_bytes = 1024;
    config.scopes = 1;
    config.metric_bytes = stats.metric_capacity * 2;
    ta_otlp *moved = NULL;
    size_t bytes = 0;
    void *memory = NULL;
    if (stats.metric_capacity < TA_OTLP_STAGING_BYTES_MAX / 2 &&
        ta_otlp_layout(&config, &bytes) == TA_OK && (memory = malloc(bytes)) != NULL &&
        ta_otlp_relocate(otlp, memory, bytes, &config, &moved) == TA_OK) {
        free(otlp);
        *length = bytes;
        return moved;
    }
    free(memory);
    free(otlp);
    return NULL;
}

/* The request of `metrics` (an array as above) and, with a host, its stats;
 * the resource is [[key, value]]; times in ms since the epoch. */
static napi_value metrics_request(napi_env environment, nd_system *host, napi_value metrics,
                                  napi_value resource, double start_ms, double time_ms) {
    uint64_t start_ns = start_ms > 0 ? (uint64_t)(start_ms * 1e6) : 0,
             time_ns = time_ms > 0 ? (uint64_t)(time_ms * 1e6) : 0;
    size_t length = 0;
    ta_otlp *otlp = metrics_encoder(262144, &length);
    if (!otlp) goto refused;
    otlp_resource_set(environment, otlp, resource);
    if (host) {
        uint32_t chart_count = ta_host_chart_stats(host->host, NULL, 0),
                 segments = ta_host_segment_count(host->host), system_count = 0;
        ta_chart_stats *charts =
            (ta_chart_stats *)calloc(chart_count ? chart_count : 1, sizeof(ta_chart_stats));
        ta_system_stats *systems =
            (ta_system_stats *)calloc(segments ? segments : 1, sizeof(ta_system_stats));
        for (uint32_t i = 0; i < chart_count; i++)
            charts[i].size = sizeof charts[i];
        ta_host_chart_stats(host->host, charts, chart_count);
        for (uint32_t k = 0; k < segments; k++) {
            ta_system *system = ta_host_system(host->host, k);
            if (!system) continue;
            systems[system_count].size = sizeof systems[system_count];
            ta_system_stats_get(system, &systems[system_count++]);
        }
        ta_host_stats host_stats;
        host_stats.size = sizeof host_stats;
        ta_host_stats_get(host->host, &host_stats);
        ta_otlp_stats_input input;
        memset(&input, 0, sizeof input);
        input.size = sizeof input;
        input.charts = charts;
        input.chart_count = chart_count;
        input.systems = systems;
        input.system_count = system_count;
        input.host = &host_stats;
        input.ticks_per_second = 1000; /* the bindings' clock is in ms */
        input.start_ns = start_ns;
        input.time_ns = time_ns;
        ta_status status;
        while ((status = ta_otlp_add_stats(otlp, &input)) == TA_ERROR_NO_MEMORY &&
               (otlp = metrics_grow(otlp, &length)) != NULL)
            ;
        free(charts);
        free(systems);
        if (!otlp) goto refused;
        /* the trace encoder's macrostep durations (TA_OTLP_SPAN_METRICS) */
        while (host->otlp &&
               (status = ta_otlp_add_span_metrics(otlp, host->otlp, start_ns, time_ns)) ==
                   TA_ERROR_NO_MEMORY &&
               (otlp = metrics_grow(otlp, &length)) != NULL)
            ;
        if (!otlp) goto refused;
    }
    bool array = false;
    uint32_t n = 0;
    napi_is_array(environment, metrics, &array);
    if (array) napi_get_array_length(environment, metrics, &n);
    for (uint32_t i = 0; i < n; i++) {
        napi_value metric;
        napi_get_element(environment, metrics, i, &metric);
        ta_status status;
        while ((status = metric_add(environment, otlp, metric, start_ns, time_ns)) ==
                   TA_ERROR_NO_MEMORY &&
               (otlp = metrics_grow(otlp, &length)) != NULL)
            ;
        if (!otlp) goto refused;
        if (status != TA_OK) {
            free(otlp);
            napi_throw_type_error(environment, NULL, "metricsToOTLP: a metric is malformed");
            return NULL;
        }
    }
    size_t size = ta_otlp_metrics_size(otlp);
    unsigned char *bytes = (unsigned char *)malloc(size ? size : 1);
    if (!bytes) {
        free(otlp);
        goto refused;
    }
    if (size) ta_otlp_put_metrics(otlp, bytes, size);
    napi_value out = bytes_value(environment, bytes, size);
    free(bytes);
    free(otlp);
    return out;
refused:
    napi_throw_error(environment, NULL, "no memory for the metrics request");
    return NULL;
}

/* metrics(host, metrics, resource, startMs, timeMs) -> Uint8Array: the host's
 * stats and `metrics` */
napi_value js_metrics(napi_env environment, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 5 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    double start = 0, time = 0;
    napi_get_value_double(environment, argv[3], &start);
    napi_get_value_double(environment, argv[4], &time);
    return metrics_request(environment, host, argv[1], argv[2], start, time);
}

/* metricsToOTLP(metrics, resource, startMs, timeMs) -> Uint8Array */
napi_value js_metrics_to_otlp(napi_env environment, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (argc < 4) return NULL;
    double start = 0, time = 0;
    napi_get_value_double(environment, argv[2], &start);
    napi_get_value_double(environment, argv[3], &time);
    return metrics_request(environment, NULL, argv[0], argv[1], start, time);
}

static napi_value doubles_value(napi_env environment, ta_string_view packed, int counts) {
    napi_value list;
    uint32_t n = packed.length / 8u;
    napi_create_array_with_length(environment, n, &list);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t bits;
        memcpy(&bits, packed.pointer + 8u * i, 8);
        double d;
        if (counts)
            d = (double)bits;
        else
            memcpy(&d, &bits, sizeof d);
        napi_set_element(environment, list, i, num(environment, d));
    }
    return list;
}

/* decodeMetrics(bytes) -> [{ name, unit, kind, points: [...] }], or throws */
napi_value js_decode_metrics(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], metrics;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *bytes = NULL;
    size_t length = 0;
    if (argc < 1 || !bytes_of(environment, argv[0], &bytes, &length)) {
        napi_throw_type_error(environment, NULL, "decodeMetrics(bytes: Uint8Array)");
        return NULL;
    }
    napi_create_array(environment, &metrics);
    ta_otlp_cursor cursor;
    memset(&cursor, 0, sizeof cursor);
    ta_otlp_metric_view view;
    int32_t got;
    uint32_t i = 0;
    while ((got = ta_otlp_next_metric(bytes, length, &cursor, &view)) == 1) {
        napi_value o, points;
        napi_create_object(environment, &o);
        napi_set_named_property(environment, o, "name", str_value(environment, view.name));
        napi_set_named_property(environment, o, "unit", str_value(environment, view.unit));
        napi_set_named_property(environment, o, "kind", num(environment, view.kind));
        napi_create_array(environment, &points);
        uint32_t at = 0, k = 0;
        ta_otlp_point_view point;
        while ((got = ta_otlp_next_point(view.points, &at, view.kind, &point)) == 1) {
            napi_value p, exemplars;
            napi_create_object(environment, &p);
            napi_set_named_property(environment, p, "attributes",
                                    decoded_attributes(environment, point.attributes));
            if (view.kind == TA_OTLP_METRIC_HISTOGRAM) {
                napi_set_named_property(environment, p, "count",
                                        num(environment, (double)point.count));
                napi_set_named_property(environment, p, "sum", num(environment, point.sum));
                if (point.has_min_max) {
                    napi_set_named_property(environment, p, "min", num(environment, point.min));
                    napi_set_named_property(environment, p, "max", num(environment, point.max));
                }
                napi_set_named_property(environment, p, "bounds",
                                        doubles_value(environment, point.bounds, 0));
                napi_set_named_property(environment, p, "buckets",
                                        doubles_value(environment, point.buckets, 1));
                napi_create_array(environment, &exemplars);
                uint32_t exemplar_at = 0, e = 0;
                ta_otlp_exemplar exemplar;
                while (ta_otlp_next_exemplar(point.exemplars, &exemplar_at, &exemplar) == 1) {
                    napi_value x;
                    napi_create_object(environment, &x);
                    napi_set_named_property(
                        environment, x, "traceID",
                        decoded_id(environment, exemplar.trace.high, exemplar.trace.low, 1));
                    napi_set_named_property(environment, x, "spanID",
                                            decoded_id(environment, 0, exemplar.span, 0));
                    napi_set_named_property(environment, x, "time",
                                            num(environment, (double)exemplar.time / 1e6));
                    napi_set_named_property(environment, x, "value",
                                            num(environment, exemplar.value));
                    napi_set_element(environment, exemplars, e++, x);
                }
                napi_set_named_property(environment, p, "exemplars", exemplars);
            } else {
                napi_set_named_property(environment, p, "value",
                                        num(environment, point.flags & TA_OTLP_POINT_DOUBLE
                                                             ? point.number
                                                             : (double)point.integer));
            }
            napi_set_element(environment, points, k++, p);
        }
        if (got < 0) break;
        napi_set_named_property(environment, o, "points", points);
        napi_set_element(environment, metrics, i++, o);
    }
    if (got < 0) {
        napi_throw_error(environment, NULL, "decodeMetrics: malformed OTLP protobuf");
        return NULL;
    }
    return metrics;
}
