/* export.c — OTLP/HTTP bodies for core.mjs to post (ta_otlp_batch, docs/adr/0008): the
 * batching, the cap and the counts are core's; JavaScript only posts (binding.h). */
#include "binding.h"

static const char *const signal_names[TA_OTLP_SIGNALS] = {"traces", "logs", "metrics"};

static int signal_of(napi_env environment, napi_value v) {
    size_t length = 0;
    char *name =
        type_of(environment, v) == napi_string ? get_string(environment, v, &length) : NULL;
    int signal = -1;
    for (int i = 0; name && i < (int)TA_OTLP_SIGNALS; i++)
        if (!strcmp(name, signal_names[i])) signal = i;
    free(name);
    return signal;
}

void exporter_free(nd_system *system) {
    ta_otlp_batch_free(system->exporter);
    system->exporter = NULL;
}

/* bytes of a signal for the exporter, when it posts that signal */
void exporter_add(nd_system *system, uint32_t signal, const void *bytes, size_t length) {
    if (system->exporter && length && (system->export_signals & (1u << signal)))
        ta_otlp_batch_add(system->exporter, signal, bytes, length, links_now(system));
}

/* the bodies due (all of them when flushing), as effects { kind: 'post', signal, body } */
void exporter_output(nd_system *system) {
    if (!system->exporter) return;
    napi_env environment = system->environment;
    uint32_t signal;
    ta_string_view body;
    while (ta_otlp_batch_next(system->exporter, links_now(system), system->export_flush, &signal,
                              &body)) {
        napi_value e = effect_new(system, "post");
        set(environment, e, "signal",
            str_value(environment, view_of(signal_names[signal], strlen(signal_names[signal]))));
        set(environment, e, "body", bytes_value(environment, body.pointer, body.length));
    }
    system->export_flush = 0;
}

ta_time exporter_deadline(nd_system *system) {
    return system->exporter ? ta_otlp_batch_deadline(system->exporter) : UINT64_MAX;
}

/* exporter(system, { traces, logs, metrics }): which signals core.mjs posts (none: no exporter) */
napi_value js_exporter(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    uint32_t signals = 0;
    for (uint32_t i = 0; i < TA_OTLP_SIGNALS; i++)
        if (property_bool(environment, argv[1], signal_names[i])) signals |= 1u << i;
    system->export_signals = signals;
    if (!signals) {
        exporter_free(system);
        return undef(environment);
    }
    ta_allocator allocator = ta_runtime_malloc();
    if (!system->exporter && ta_otlp_batch_new(&allocator, &system->exporter) != TA_OK) {
        napi_throw_error(environment, NULL, "cannot create the exporter");
        return NULL;
    }
    return undef(environment);
}

/* exportAdd(system, signal, bytes): bytes for the exporter (metrics) */
napi_value js_export_add(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    int signal = signal_of(environment, argv[1]);
    void *data = NULL;
    size_t n = 0;
    if (signal >= 0 && bytes_of(environment, argv[2], &data, &n))
        exporter_add(system, (uint32_t)signal, data, n);
    return input_done(system, undef(environment)); /* posts due go out */
}

/* exportFlush(system): post everything waiting now */
napi_value js_export_flush(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    system->export_flush = 1;
    return input_done(system, undef(environment));
}

/* posted(system, signal, ok) -> the failures so far when one is worth a log line now, else 0 */
napi_value js_posted(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    int signal = signal_of(environment, argv[1]);
    if (!system->exporter || signal < 0) return num(environment, 0);
    bool ok = false;
    napi_get_value_bool(environment, argv[2], &ok);
    ta_time now = links_now(system);
    ta_otlp_batch_done(system->exporter, (uint32_t)signal, ok, now);
    uint64_t failures = 0;
    int report = ta_otlp_batch_report(system->exporter, now, &failures);
    return input_done(system,
                      num(environment, report ? (double)failures : 0)); /* what waited goes */
}

/* exportStats(system) -> { posted, failed, dropped, waiting, inFlight } (bytes) */
napi_value js_export_stats(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], o;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    ta_otlp_batch_stats stats;
    memset(&stats, 0, sizeof stats);
    stats.size = sizeof stats;
    if (system->exporter) ta_otlp_batch_stats_get(system->exporter, &stats);
    napi_create_object(environment, &o);
    set(environment, o, "posted", num(environment, (double)stats.posted));
    set(environment, o, "failed", num(environment, (double)stats.failed));
    set(environment, o, "dropped", num(environment, (double)stats.dropped));
    set(environment, o, "waiting", num(environment, (double)stats.waiting));
    set(environment, o, "inFlight", num(environment, stats.in_flight));
    return o;
}

/* exportDrain(system) -> [{ signal, body }]: everything waiting, for a system that goes */
napi_value js_export_drain(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    napi_create_array(environment, &out);
    uint32_t signal, k = 0;
    ta_string_view body;
    ta_time now = links_now(system);
    while (system->exporter && ta_otlp_batch_next(system->exporter, now, 1, &signal, &body)) {
        napi_value e;
        napi_create_object(environment, &e);
        set(environment, e, "signal",
            str_value(environment, view_of(signal_names[signal], strlen(signal_names[signal]))));
        set(environment, e, "body", bytes_value(environment, body.pointer, body.length));
        napi_set_element(environment, out, k++, e);
        ta_otlp_batch_done(system->exporter, signal, 1, now);
    }
    return out;
}
