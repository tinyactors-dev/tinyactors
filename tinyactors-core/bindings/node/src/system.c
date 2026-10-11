/* system.c — a System: create, the run loop, effects, spawn, send, deliver, inspection (binding.h).
 */
#include "binding.h"

void grow(void **p, uint32_t *capacity, uint32_t need, size_t size) {
    if (need <= *capacity) return;
    uint32_t c = *capacity ? *capacity : 16;
    while (c < need)
        c *= 2;
    void *q = realloc(*p, size * c);
    if (!q) abort(); /* the bindings' own tables: nothing sensible to do without memory */
    memset((char *)q + size * *capacity, 0, size * (c - *capacity));
    *p = q;
    *capacity = c;
}

void mark_changed(nd_system *system, uint32_t *stamp, uint64_t serial) {
    if (*stamp == system->pump || !serial) return;
    *stamp = system->pump;
    grow((void **)&system->changed, &system->changed_capacity, system->changed_count + 1,
         sizeof(uint64_t));
    system->changed[system->changed_count++] = serial;
}

uint64_t serial_of(ta_string_view sessionid) {
    uint64_t serial = 0;
    for (uint32_t k = 0; k < sessionid.length; k++)
        if (sessionid.pointer[k] >= '0' && sessionid.pointer[k] <= '9')
            serial = serial * 10 + (uint64_t)(sessionid.pointer[k] - '0');
    return serial;
}

void grave_add(nd_system *system, uint64_t serial, napi_value context) {
    if (system->keep_quiet || !context) return;
    grow((void **)&system->graves, &system->grave_capacity, system->grave_count + 1,
         sizeof(nd_grave));
    system->graves[system->grave_count].serial = serial;
    system->graves[system->grave_count++].context = make_ref(system->environment, context);
}

/* Record a pending JS exception against a session, and clear it. */
void take_exception(nd_system *system, uint64_t serial) {
    bool pending = false;
    napi_is_exception_pending(system->environment, &pending);
    if (!pending) return;
    napi_value exception;
    napi_get_and_clear_last_exception(system->environment, &exception);
    grow((void **)&system->failures, &system->failure_capacity, system->failure_count + 1,
         sizeof(nd_failure));
    system->failures[system->failure_count].serial = serial;
    system->failures[system->failure_count++].error = make_ref(system->environment, exception);
}

void chart_add(nd_system *system, nd_chart *chart) {
    grow((void **)&system->charts, &system->chart_capacity, system->chart_count + 1,
         sizeof(nd_chart *));
    system->charts[system->chart_count++] = chart;
}

void chart_remove(nd_system *system, nd_chart *chart) {
    for (uint32_t i = 0; i < system->chart_count; i++)
        if (system->charts[i] == chart) system->charts[i] = system->charts[--system->chart_count];
}

nd_chart *chart_new(const ta_datamodel_chart *chart) {
    nd_chart *c = (nd_chart *)calloc(1, sizeof *c);
    if (!c) return NULL;
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(chart->image, &info);
    c->image = chart->image;
    c->chart = chart->chart;
    c->hash = info.content_hash;
    c->count = ta_image_expression_count(chart->image, chart->chart);
    return c;
}

/* ---- values between JavaScript and the datamodel ------------------------------------------ */

/* A payload (of the system's datamodel) as a JS value; undefined for none. */
static napi_value value_of_payload(nd_system *system, ta_payload payload, uint32_t datamodel) {
    napi_env environment = system->environment;
    if (!payload || datamodel == TA_NIL) return undef(environment);
    if (system->native) return payload_get(system, payload);
    size_t length = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!text_room(&system->wire, system->wire.capacity ? 0 : 4096)) break;
        ta_status status = Q(system)->to_wire(system->quickjs, payload, system->wire.bytes,
                                              system->wire.capacity, &length);
        if (status == TA_OK) {
            napi_value v = value_of_wire(environment, view_of(system->wire.bytes, length),
                                         get_ref(environment, system->refs));
            return v ? v : undef(environment);
        }
        if (status != TA_ERROR_NO_MEMORY || !text_room(&system->wire, length + 1)) break;
    }
    return undef(environment);
}

/* A JS value as a new payload (one reference); 0 for undefined, or when it
 * is not a neutral value for the quickjs datamodel (thrown). */
static ta_payload payload_of_value(nd_system *system, napi_value v) {
    napi_env environment = system->environment;
    if (type_of(environment, v) == napi_undefined) return 0;
    if (system->native) return payload_new(system, v);
    ta_payload out = 0;
    if (wire_of(environment, v, &system->wire) != TA_OK ||
        Q(system)->from_wire(system->quickjs, view_of(system->wire.bytes, system->wire.length),
                             &out) != TA_OK) {
        napi_throw_type_error(environment, NULL,
                              "event data must be a neutral value (docs/protocol.md)");
        return 0;
    }
    return out;
}

static ta_payload payload_of_wire(nd_system *system, ta_string_view json) {
    ta_payload out = 0;
    system->binding.vtable->from_wire(system, json, &out);
    return out;
}

static void payload_drop(nd_system *system, ta_payload payload) {
    if (payload) system->binding.vtable->release(system, payload);
}

/* A payload as neutral JSON in system->wire (empty: none or not neutral). */

/* ---- System handles ------------------------------------------------------------------------ */

static void system_clear(napi_env environment, nd_system *system) {
    system->environment = environment;
    for (uint32_t i = 0; i < system->instance_count; i++)
        if (system->instances[i].live) instance_free(system, i + 1);
    free(system->instances);
    for (uint32_t i = 0; i < system->payload_count; i++)
        if (system->payload_refs[i]) napi_delete_reference(environment, system->payloads[i]);
    free(system->payloads);
    free(system->payload_refs);
    for (uint32_t i = 0; i < system->image_count; i++)
        set_ref(environment, &system->images[i], NULL);
    free(system->images);
    for (uint32_t i = 0; i < system->grave_count; i++)
        set_ref(environment, &system->graves[i].context, NULL);
    free(system->graves);
    for (uint32_t i = 0; i < system->failure_count; i++)
        set_ref(environment, &system->failures[i].error, NULL);
    free(system->failures);
    free(system->charts);
    free(system->keys);
    free(system->sessions);
    free(system->changed);
    parked_clear(system);
    jobs_clear(system);
    links_free(system);
    exporter_free(system);
    for (uint32_t i = 0; i < system->later_count; i++)
        set_ref(environment, &system->later[i], NULL);
    free(system->later);
    for (uint32_t i = 0; i < 2 * IO_PROCESSORS_MAX; i++)
        free(system->io_text[i]);
    set_ref(environment, &system->resolve, NULL);
    set_ref(environment, &system->effects, NULL);
    set_ref(environment, &system->sync, NULL);
    set_ref(environment, &system->refs, NULL);
    set_ref(environment, &system->clock, NULL);
    set_ref(environment, &system->schedule, NULL);
    set_ref(environment, &system->now, NULL);
    set_ref(environment, &system->in_function, NULL);
    free(system->wire.bytes);
    free(system->otlp);
    free(system->traces);
    free(system->logs);
    memset(system, 0, sizeof *system);
}

/* Free the host and every reference now: callbacks can close over the JS
 * owner of this handle, so waiting for finalization would keep both alive. */
static void system_destroy(napi_env environment, nd_system *system) {
    if (!system->host && !system->quickjs && !system->resolve) return;
    system->environment = environment;
    system->keep_quiet = 1;
    if (system->host) ta_host_free(system->host);
    system->host = NULL;
    if (system->quickjs) ta_datamodel_ecmascript_free(system->quickjs);
    system_clear(environment, system);
}

void system_finalize(napi_env environment, void *data, void *hint) {
    (void)hint;
    nd_system *system = (nd_system *)data;
    system_destroy(environment, system);
    free(system);
}

nd_system *get_system(napi_env environment, napi_value v) {
    void *p = NULL;
    if (type_of(environment, v) != napi_external ||
        napi_get_value_external(environment, v, &p) != napi_ok || !p) {
        napi_throw_type_error(environment, NULL, "expected a system handle");
        return NULL;
    }
    nd_system *system = (nd_system *)p;
    if (!system->host || system->stopping) {
        napi_throw_error(environment, NULL, "the system was stopped");
        return NULL;
    }
    if (system->running == 1) {
        napi_throw_error(environment, NULL,
                         "the system is running (an expression cannot call into its system)");
        return NULL;
    }
    system->environment = environment;
    return system;
}

/* later(system, fn) -> true when called from inside a run (an expression
 * calling into its system): fn is called between two runs, in order with the
 * others; false otherwise (the caller goes on at once). */
napi_value js_later(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *p = NULL;
    if (argc < 2 || type_of(environment, argv[0]) != napi_external ||
        napi_get_value_external(environment, argv[0], &p) != napi_ok || !p)
        return bool_value(environment, 0);
    nd_system *system = (nd_system *)p;
    if (system->running != 1 || !system->host || system->stopping)
        return bool_value(environment, 0);
    grow((void **)&system->later, &system->later_capacity, system->later_count + 1,
         sizeof(napi_ref));
    system->later[system->later_count++] = make_ref(environment, argv[1]);
    return bool_value(environment, 1);
}

/* The calls kept during the last run, in order; what one throws is `*thrown`. */
static void call_later(nd_system *system, napi_value *thrown) {
    napi_env environment = system->environment;
    for (uint32_t k = 0; k < system->later_count; k++) { /* calls made meanwhile go in at once */
        napi_value f = get_ref(environment, system->later[k]);
        set_ref(environment, &system->later[k], NULL);
        if (!call(environment, f, 0, NULL)) {
            if (!*thrown)
                napi_get_and_clear_last_exception(environment, thrown);
            else
                clear_exception(environment);
        }
    }
    system->later_count = 0;
}

/* ---- the run loop -------------------------------------------------------------------------- */

static napi_value effect_into(nd_system *system, const char *kind, napi_value *list,
                              uint32_t *count) {
    napi_env environment = system->environment;
    napi_value e;
    napi_create_object(environment, &e);
    set(environment, e, "kind", str_value(environment, view_of(kind, strlen(kind))));
    if (!*list) napi_create_array(environment, list);
    napi_set_element(environment, *list, (*count)++, e);
    return e;
}

napi_value effect_new(nd_system *system, const char *kind) {
    return effect_into(system, kind, &system->effect_list, &system->effect_count);
}

static napi_value traceparent_value(napi_env environment, const ta_trace_context *trace) {
    char t[TA_TRACEPARENT_LENGTH];
    uint32_t n = ta_traceparent_format(trace, t, sizeof t);
    return n ? str_value(environment, view_of(t, n)) : undef(environment);
}

/* TA_EFFECT_REQUEST: an <invoke src> that is not a chart of its image: the
 * chart of that source name in the segment's image, if there is one */
static void answer_request(nd_system *system, const ta_host_effect *effect) {
    uint32_t chart = TA_NIL;
    if (effect->segment < system->image_count && system->images[effect->segment]) {
        void *p = NULL;
        napi_get_value_external(system->environment,
                                get_ref(system->environment, system->images[effect->segment]), &p);
        const ta_image *image = p ? &((nd_image *)p)->image : NULL;
        ta_string_view want = source_name(effect->auxiliary);
        ta_image_info info;
        info.size = sizeof info;
        if (image) ta_image_get_info(image, &info);
        for (uint32_t c = 0; image && c < info.chart_count && chart == TA_NIL; c++) {
            ta_string_view source = ta_image_chart_source(image, c);
            if (source.length == want.length && !memcmp(source.pointer, want.pointer, want.length))
                chart = c;
        }
        if (image && chart == TA_NIL) chart = ta_image_chart_by_name(image, want);
    }
    ta_host_resume(system->host, effect->segment, effect->token,
                   chart == TA_NIL ? TA_ERROR_INVALID : TA_OK,
                   chart == TA_NIL ? 0 : (ta_payload)chart + 1);
}

static napi_value grave_context(nd_system *system, uint64_t serial) {
    for (uint32_t i = 0; i < system->grave_count; i++)
        if (system->graves[i].serial == serial)
            return get_ref(system->environment, system->graves[i].context);
    return undef(system->environment);
}

static void effect_out(nd_system *system, const ta_host_effect *effect) {
    napi_env environment = system->environment;
    napi_value e;
    switch (effect->kind) {
    case TA_EFFECT_LOG:
        e = effect_new(system, "log");
        set(environment, e, "session", num(environment, (double)effect->session));
        set(environment, e, "root", num(environment, (double)effect->root));
        set(environment, e, "label", str_value(environment, effect->auxiliary));
        set(environment, e, "text", str_value(environment, effect->event));
        break;
    case TA_EFFECT_DONE:
        e = effect_new(system, "done");
        set(environment, e, "session", num(environment, (double)effect->session));
        set(environment, e, "final", str_value(environment, effect->auxiliary));
        set(environment, e, "output", value_of_payload(system, effect->payload, effect->datamodel));
        set(environment, e, "context", grave_context(system, effect->session));
        break;
    case TA_EFFECT_ERROR:
        e = effect_new(system, "error");
        set(environment, e, "session", num(environment, (double)effect->session));
        set(environment, e, "code", num(environment, effect->flags));
        set(environment, e, "context", grave_context(system, effect->session));
        break;
    case TA_EFFECT_SEND:
        /* a synchronous processor's send is answered before the next run (pump) */
        e = effect->io_processor < system->io_count &&
                    (system->io[effect->io_processor].flags & TA_IO_PROCESSOR_SYNC)
                ? effect_into(system, "send", &system->sync_list, &system->sync_count)
                : effect_new(system, "send");
        set(environment, e, "session", num(environment, (double)effect->session));
        set(environment, e, "root", num(environment, (double)effect->root));
        set(environment, e, "segment", num(environment, effect->segment));
        set(environment, e, "token", num(environment, effect->token));
        set(environment, e, "ioProcessor",
            num(environment, effect->io_processor == TA_NIL ? -1.0 : (double)effect->io_processor));
        set(environment, e, "target", str_value(environment, effect->target));
        set(environment, e, "event", str_value(environment, effect->event));
        set(environment, e, "sendid", str_value(environment, effect->sendid));
        set(environment, e, "data", value_of_payload(system, effect->payload, effect->datamodel));
        set(environment, e, "traceparent", traceparent_value(environment, &effect->trace));
        break;
    case TA_EFFECT_INVOKE:
        e = effect_new(system, "invoke");
        set(environment, e, "session", num(environment, (double)effect->session));
        set(environment, e, "segment", num(environment, effect->segment));
        set(environment, e, "token", num(environment, effect->token));
        set(environment, e, "type", str_value(environment, effect->event));
        set(environment, e, "src", str_value(environment, effect->target));
        set(environment, e, "invokeid", str_value(environment, effect->sendid));
        set(environment, e, "params", value_of_payload(system, effect->payload, effect->datamodel));
        set(environment, e, "traceparent", traceparent_value(environment, &effect->trace));
        break;
    case TA_EFFECT_UNINVOKE:
        e = effect_new(system, "uninvoke");
        set(environment, e, "session", num(environment, (double)effect->session));
        set(environment, e, "segment", num(environment, effect->segment));
        set(environment, e, "token", num(environment, effect->token));
        break;
    case TA_EFFECT_WAKE: /* the caller loads the blob, unless it is still held */
        if (wake_effect(system, effect->session)) {
            e = effect_new(system, "wake");
            set(environment, e, "session", num(environment, (double)effect->session));
        }
        break;
    case TA_EFFECT_REQUEST:
        answer_request(system, effect);
        break;
    case TA_HOST_EFFECT_JOB_DONE: /* migrate()'s promise */
        job_done(system, effect);
        break;
    case TA_HOST_EFFECT_SEGMENT_FREED:
        if (effect->segment < system->image_count)
            set_ref(environment, &system->images[effect->segment], NULL);
        e = effect_new(system, "freed");
        set(environment, e, "segment", num(environment, effect->segment));
        break;
    default:
        break;
    }
}

#define RUN_BUDGET 10000u
#define RUN_ROUNDS 16

/* One pass: ta_host_run in rounds until idle (or RUN_ROUNDS runs); every
 * effect, the changed sessions and the traces go to callbacks.effects at once.
 * Returns the run code, NaN when callbacks.effects threw. */
static double pump_once(nd_system *system) {
    napi_env environment = system->environment;
    double now = system_now(system);
    ta_host_advance_time(system->host, now > 0 ? (ta_time)now : 0);
    if (system->links) ta_links_time(system->links, now > 0 ? (ta_time)now : 0);
    system->running = 1;
    system->pump++;
    system->effect_list = system->sync_list = NULL;
    system->effect_count = system->sync_count = 0;
    system->changed_count = 0;
    napi_value thrown = NULL; /* by callbacks.sync: rethrown once the pump is done */
    ta_run result;
    memset(&result, 0, sizeof result);
#if defined(__wasm__)
    if (system->quickjs) /* the web module has no clock of its own (browser_wasm.c) */
        ta_datamodel_ecmascript_wasm_set_time((double)trace_wall(system) / 1e6);
#endif
    for (int round = 0; round < RUN_ROUNDS; round++) {
        if (system->otlp) ta_otlp_mark(system->otlp, trace_fuel(system), trace_wall(system));
        result = ta_host_run(system->host, RUN_BUDGET);
        if (system->otlp) ta_otlp_mark(system->otlp, trace_fuel(system), trace_wall(system));
        trace_drain(system); /* before the effects: what they cause comes after */
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(system->host, &effects);
        for (uint32_t i = 0; i < n; i++)
            if (!system->links || !ta_links_effect(system->links, &effects[i], (ta_time)now))
                effect_out(system, &effects[i]);
        ta_host_effects_ack(system->host);
        if (system->links && ta_links_deadline(system->links) <= (ta_time)now) {
            ta_links_time(system->links, (ta_time)now); /* refused envelopes: errors back */
            system->more = 1;
        }
        if (system->wake_held_count) wake_held(system); /* their work runs in the next round */
        /* TA_RUN_EFFECTS: the senders wait; callbacks.sync answers each send now,
         * feeding the host (send, post, deliver) before the next run */
        napi_value sync_list = system->sync_list;
        uint32_t sync_count = system->sync_count;
        system->sync_list = NULL;
        system->sync_count = 0;
        system->running = 3;
        for (uint32_t i = 0; i < sync_count && !thrown; i++) {
            napi_value e;
            napi_get_element(environment, sync_list, i, &e);
            if (!call(environment, get_ref(environment, system->sync), 1, &e))
                napi_get_and_clear_last_exception(environment, &thrown);
        }
        if (system->later_count && !thrown) call_later(system, &thrown); /* (running 3) */
        system->running = 1;
        if (thrown || system->stopping || result.status == TA_RUN_IDLE ||
            result.status == TA_RUN_FAULT)
            break;
    }
    for (uint32_t i = 0; i < system->failure_count; i++) { /* exceptions in expressions */
        napi_value e = effect_new(system, "exception");
        set(environment, e, "session", num(environment, (double)system->failures[i].serial));
        set(environment, e, "error", get_ref(environment, system->failures[i].error));
        set_ref(environment, &system->failures[i].error, NULL);
    }
    system->failure_count = 0;
    for (uint32_t i = 0; i < system->grave_count; i++)
        set_ref(environment, &system->graves[i].context, NULL);
    system->grave_count = 0;
    napi_value changed = NULL;
    if (system->changed_count) {
        napi_create_array_with_length(environment, system->changed_count, &changed);
        for (uint32_t i = 0; i < system->changed_count; i++)
            napi_set_element(environment, changed, i, num(environment, (double)system->changed[i]));
    }
    if (system->otlp) trace_put(system); /* the spans that ended in this call */
    exporter_add(system, TA_OTLP_TRACES, system->traces, system->traces_length);
    exporter_add(system, TA_OTLP_LOGS, system->logs, system->logs_length);
    napi_value traces = system->trace_sink && system->traces_length
                            ? bytes_value(environment, system->traces, system->traces_length)
                            : NULL;
    napi_value logs = system->trace_sink && system->logs_length
                          ? bytes_value(environment, system->logs, system->logs_length)
                          : NULL;
    system->traces_length = system->logs_length = 0;
    links_output(system);    /* what the transports are to do */
    exporter_output(system); /* and the exporter's posts */
    napi_value list = system->effect_list;
    system->effect_list = NULL;
    system->running = 0;
    if (list || changed || traces || logs) {
        napi_value argv[4] = {
            list ? list : undef(environment), changed ? changed : undef(environment),
            traces ? traces : undef(environment), logs ? logs : undef(environment)};
        system->running = 2; /* inputs from the handlers go in: the pump goes on */
        napi_value r = call(environment, get_ref(environment, system->effects), 4, argv);
        system->running = 0;
        if (!r) return NAN; /* the exception propagates */
    }
    if (thrown) {
        napi_throw(environment, thrown);
        return NAN;
    }
    if (result.status != TA_RUN_IDLE) return -2;
    return result.next_deadline == UINT64_MAX ? -1 : (double)result.next_deadline;
}

/* Arm the caller's one timer for a run code (callbacks.schedule): 0 to run
 * soon, -1 for nothing to do, else the delay to the next deadline. A caller's
 * own clock (callbacks.now) arms nothing: time moves when it pumps. */
static void arm(nd_system *system, double code) {
    if (system->now || !system->schedule || system->stopping) return;
    if (code == system->armed) return;
    system->armed = code;
    double delay = code == -2 ? 0 : code < 0 ? -1 : code - system_now(system);
    napi_value argv[1] = {num(system->environment, delay < 0 && code >= 0 ? 0 : delay)};
    call(system->environment, get_ref(system->environment, system->schedule), 1, argv);
}

static const double run_bounds[RUN_BUCKETS - 1] = {0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25};

static void run_record(nd_system *system, double seconds) {
    uint32_t bucket = 0;
    while (bucket < RUN_BUCKETS - 1 && seconds > run_bounds[bucket])
        bucket++;
    system->run_buckets[bucket]++;
    system->run_min = system->run_count && system->run_min < seconds ? system->run_min : seconds;
    system->run_max = system->run_count && system->run_max > seconds ? system->run_max : seconds;
    system->run_count++;
    system->run_sum += seconds;
}

/* Run until idle: passes of RUN_ROUNDS runs, again while effect handlers
 * gave the host input (at most RUN_ROUNDS passes, then the timer goes on);
 * then the timer is armed. Returns the run code; NaN when an effect handler
 * threw (the exception is pending). Inside a pump it only notes more work. */
double pump(nd_system *system) {
    if (system->running) {
        system->more = 1;
        return -2;
    }
    uint64_t started = ta_runtime_now_ns();
    double code = -2;
    for (int pass = 0; pass < RUN_ROUNDS; pass++) {
        system->more = 0;
        code = pump_once(system);
        if (code != code || system->stopping || !system->more) break;
        code = -2;
    }
    run_record(system, (double)(ta_runtime_now_ns() - started) / 1e9);
    if (system->stopping) {
        system_destroy(system->environment, system);
        return -1;
    }
    if (code == code) { /* pings, reconnects, resumption windows; posts due */
        ta_time links = system->links ? ta_links_deadline(system->links) : UINT64_MAX;
        ta_time posts = exporter_deadline(system);
        ta_time due = links < posts ? links : posts;
        if (due != UINT64_MAX && (code == -1 || (code >= 0 && (double)due < code)))
            code = (double)due;
    }
    if (code == code) arm(system, system->more ? -2 : code);
    return code;
}

/* After an input: from outside, the pump runs now; from an effect handler or
 * a synchronous send, the pump that is running goes on. Returns `result`, or
 * NULL when the pump threw. */
napi_value input_done(nd_system *system, napi_value result) {
    if (system->running) {
        system->more = 1;
        return result;
    }
    pump(system);
    bool pending = false;
    napi_is_exception_pending(system->environment, &pending);
    return pending ? NULL : result;
}

/* A host call that runs JavaScript (charts attach, blobs are written): no calls in meanwhile. */
int enter(nd_system *system) {
    int was = system->running;
    system->running = 1;
    return was;
}

void leave(nd_system *system, int was) {
    system->running = was;
}

/* The system's time in ms: the caller's clock (callbacks.now), else the monotonic one. */
double system_now(nd_system *system) {
    if (system->now) {
        napi_value r =
            call(system->environment, get_ref(system->environment, system->now), 0, NULL);
        double ms = r && type_of(system->environment, r) == napi_number
                        ? get_double(system->environment, r)
                        : 0;
        return ms > 0 ? ms : 0;
    }
    return (double)(ta_runtime_now_ns() - system->time_base) / 1e6;
}

/* ---- creating a system ---------------------------------------------------------------------- */

/* config: { systemID, ecmascript: 'native' | 'quickjs', ioProcessors: [{ type, location, sync }],
 * maxActors, maxEvents, maxTimers, stringBytes, seed: [high, low], maxOps, memoryLimit } */
nd_system *system_new(napi_env environment, napi_value callbacks, napi_value options,
                      ta_host_config *config) {
    size_t length = 0;
    char *id = property_string(environment, options, "systemID", &length);
    if (!id || !ta_address_system_valid(view_of(id, length))) {
        free(id);
        napi_throw_type_error(environment, NULL,
                              "createSystem: systemID is not a valid system id (docs/protocol.md)");
        return NULL;
    }
    nd_system *system = (nd_system *)calloc(1, sizeof *system);
    if (!system) abort();
    system->environment = environment;
    memcpy(system->system_id, id, length);
    free(id);
    system->resolve = make_ref(environment, property(environment, callbacks, "resolve"));
    system->effects = make_ref(environment, property(environment, callbacks, "effects"));
    system->sync = make_ref(environment, property(environment, callbacks, "sync"));
    system->refs = make_ref(environment, property(environment, callbacks, "ref"));
    if (type_of(environment, property(environment, callbacks, "clock")) == napi_function)
        system->clock = make_ref(environment, property(environment, callbacks, "clock"));
    if (type_of(environment, property(environment, callbacks, "schedule")) == napi_function)
        system->schedule = make_ref(environment, property(environment, callbacks, "schedule"));
    if (type_of(environment, property(environment, callbacks, "now")) == napi_function)
        system->now = make_ref(environment, property(environment, callbacks, "now"));
    system->armed = -3;
    double start = property_double(environment, options, "now"); /* a restored system's time */
    system->time_base = ta_runtime_now_ns() - (uint64_t)(start > 0 ? start * 1e6 : 0);
    napi_value in;
    napi_create_function(environment, "In", NAPI_AUTO_LENGTH, js_in, system, &in);
    system->in_function = make_ref(environment, in);
    size_t kl = 0;
    char *kind = property_string(environment, options, "ecmascript", &kl);
    system->native = !kind || strcmp(kind, "quickjs") != 0;
    free(kind);
    if (system->native) {
        system->binding = (ta_datamodel_binding){&native_vtable, system};
    } else {
        ta_datamodel_ecmascript_config ecmascript;
        memset(&ecmascript, 0, sizeof ecmascript);
        ecmascript.size = sizeof ecmascript;
        ecmascript.max_ops = (uint32_t)property_double(environment, options, "maxOps");
        ecmascript.memory_limit = (size_t)property_double(environment, options, "memoryLimit");
        system->quickjs = ta_datamodel_ecmascript_new(&ecmascript);
        system->quickjs_vtable = ta_datamodel_ecmascript_vtable();
        system->binding = (ta_datamodel_binding){&quickjs_vtable, system};
    }
    napi_value io = property(environment, options, "ioProcessors");
    uint32_t count = array_length(environment, io);
    for (uint32_t i = 0; i < count && i < IO_PROCESSORS_MAX; i++) {
        napi_value entry;
        size_t tl = 0, ll = 0;
        napi_get_element(environment, io, i, &entry);
        system->io_text[2 * i] = property_string(environment, entry, "type", &tl);
        system->io_text[2 * i + 1] = property_string(environment, entry, "location", &ll);
        system->io[i].type = view_of(system->io_text[2 * i], tl);
        system->io[i].location = view_of(system->io_text[2 * i + 1], ll);
        if (property_bool(environment, entry, "sync")) system->io[i].flags = TA_IO_PROCESSOR_SYNC;
        system->io_count++;
    }
    memset(config, 0, sizeof *config);
    config->size = sizeof *config;
    config->system_id = view_of(system->system_id, strlen(system->system_id));
    config->system.size = sizeof config->system;
    config->system.ticks_per_second = 1000; /* ms */
    config->system.trace_rules = 64;        /* watches add rules (docs/adr/0007) */
    double v;
    config->system.max_actors =
        (v = property_double(environment, options, "maxActors")) > 0 ? (uint32_t)v : 64;
    config->system.max_events =
        (v = property_double(environment, options, "maxEvents")) > 0 ? (uint32_t)v : 256;
    config->system.max_timers =
        (v = property_double(environment, options, "maxTimers")) > 0 ? (uint32_t)v : 64;
    config->system.string_bytes =
        (v = property_double(environment, options, "stringBytes")) > 0 ? (uint32_t)v : 32768;
    napi_value seed = property(environment, options, "seed");
    if (array_length(environment, seed) == 2) { /* the trace ID seed, as two 32-bit halves */
        napi_value high, low;
        napi_get_element(environment, seed, 0, &high);
        napi_get_element(environment, seed, 1, &low);
        config->system.trace_seed =
            (uint64_t)get_u32(environment, high) << 32 | get_u32(environment, low);
    }
    config->datamodels = &system->binding;
    config->datamodel_count = 1;
    config->io_processors = system->io_count ? system->io : NULL;
    config->io_processor_count = system->io_count;
    config->allocator = ta_runtime_malloc();
    return system;
}

napi_value system_value(napi_env environment, nd_system *system) {
    napi_value r;
    napi_create_external(environment, system, system_finalize, NULL, &r);
    return r;
}

/* createSystem(callbacks, config) -> system */
napi_value js_create_system(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error(environment, NULL, "createSystem(callbacks, config)");
        return NULL;
    }
    ta_host_config config;
    nd_system *system = system_new(environment, argv[0], argv[1], &config);
    if (!system) return NULL;
    if (ta_host_new(&config, &system->host) != TA_OK) {
        system_finalize(environment, system, NULL);
        napi_throw_error(environment, NULL, "cannot create the system");
        return NULL;
    }
    return system_value(environment, system);
}

void keep_image(nd_system *system, uint32_t segment, napi_value image) {
    grow((void **)&system->images, &system->image_capacity, segment + 1, sizeof(napi_ref));
    set_ref(system->environment, &system->images[segment], image);
    if (segment >= system->image_count) system->image_count = segment + 1;
}

/* addImage(system, image) -> segment: its charts become the current versions */
napi_value js_add_image(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    nd_image *image = system ? get_image(environment, argv[1]) : NULL;
    if (!image) return NULL;
    uint32_t segment = 0;
    int was = enter(system); /* charts attach: resolve() runs JavaScript */
    ta_status status = ta_host_add_image(system->host, &image->image, &segment);
    leave(system, was);
    if (status != TA_OK) {
        napi_throw_error(environment, NULL, "cannot add the image");
        return NULL;
    }
    keep_image(system, segment, argv[1]);
    return num(environment, segment);
}

/* chart(system, name) -> [segment, chart] of the current version, or null */
napi_value js_chart(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], r;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    size_t nl = 0;
    char *name = get_string(environment, argv[1], &nl);
    uint32_t segment = 0, chart = 0;
    ta_status status =
        name ? ta_host_chart(system->host, view_of(name, nl), &segment, &chart) : TA_ERROR_INVALID;
    free(name);
    if (status != TA_OK) return null_value(environment);
    napi_create_array_with_length(environment, 2, &r);
    napi_set_element(environment, r, 0, num(environment, segment));
    napi_set_element(environment, r, 1, num(environment, chart));
    return r;
}

napi_value js_boot(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    ta_host_advance_time(system->host, (ta_time)system_now(system));
    int was = enter(system);
    ta_status status = ta_host_boot(system->host, get_u32(environment, argv[1]));
    leave(system, was);
    if (status != TA_OK) {
        napi_throw_error(environment, NULL, "cannot boot the image");
        return NULL;
    }
    return undef(environment); /* the actors start with the next pump */
}

napi_value js_actor_by_name(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    size_t nl = 0;
    char *name = get_string(environment, argv[1], &nl);
    uint64_t s = name ? ta_host_actor_by_name(system->host, view_of(name, nl)) : 0;
    free(name);
    return num(environment, (double)s);
}

/* actors(system) -> every session, host-wide */
napi_value js_actors(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    napi_create_array(environment, &out);
    uint64_t cursor = 0, batch[256];
    uint32_t n, k = 0;
    while ((n = ta_host_actors(system->host, &cursor, batch, 256)) > 0)
        for (uint32_t i = 0; i < n; i++)
            napi_set_element(environment, out, k++, num(environment, (double)batch[i]));
    return out;
}

char *optional_string(napi_env environment, napi_value v, size_t *n) {
    *n = 0;
    return type_of(environment, v) == napi_string ? get_string(environment, v, n) : NULL;
}

/* spawn(system, segment, chart, name, input, traceparent) -> session: the
 * bootstrap runs with the next pump (the caller knows the session first) */
napi_value js_spawn(napi_env environment, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 6 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    ta_payload input = payload_of_value(system, argv[4]);
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    if (pending) return NULL;
    size_t nl = 0, tl = 0;
    char *name = optional_string(environment, argv[3], &nl),
         *traceparent = optional_string(environment, argv[5], &tl);
    uint64_t session = 0;
    ta_host_advance_time(system->host, (ta_time)system_now(system));
    ta_status status =
        ta_host_spawn(system->host, get_u32(environment, argv[1]), get_u32(environment, argv[2]),
                      view_of(name, nl), input, view_of(traceparent, tl), &session);
    free(name);
    free(traceparent);
    if (status != TA_OK) {
        payload_drop(system, input);
        napi_throw_error(environment, NULL, "cannot spawn the actor");
        return NULL;
    }
    return num(environment, (double)session); /* it starts with the next pump */
}

/* send(system, session, name, data, traceparent): one event, then the run loop */
napi_value js_send(napi_env environment, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 5 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    size_t nl = 0, tl = 0;
    char *name = get_string(environment, argv[2], &nl),
         *traceparent = optional_string(environment, argv[4], &tl);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = view_of(name, nl);
    event.traceparent = view_of(traceparent, tl);
    event.data = payload_of_value(system, argv[3]);
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    ta_status status = pending
                           ? TA_ERROR_INVALID
                           : ta_host_send(system->host, get_session(environment, argv[1]), &event);
    if (status != TA_OK) payload_drop(system, event.data);
    free(name);
    free(traceparent);
    if (pending) return NULL;
    return input_done(system, undef(environment));
}

/* sendMany(system, sessions, names, datas, traceparents) -> accepted */
napi_value js_send_many(napi_env environment, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 5 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    uint32_t n = array_length(environment, argv[1]);
    uint64_t *sessions = (uint64_t *)calloc(n + 1, sizeof(uint64_t));
    ta_event_in *events = (ta_event_in *)calloc(n + 1, sizeof(ta_event_in));
    char **texts = (char **)calloc(2 * (n + 1), sizeof(char *));
    uint32_t ready = 0;
    for (; ready < n; ready++) {
        napi_value v;
        size_t nl = 0, tl = 0;
        napi_get_element(environment, argv[1], ready, &v);
        sessions[ready] = get_session(environment, v);
        napi_get_element(environment, argv[2], ready, &v);
        texts[2 * ready] = get_string(environment, v, &nl);
        napi_get_element(environment, argv[4], ready, &v);
        texts[2 * ready + 1] = optional_string(environment, v, &tl);
        events[ready].size = sizeof events[ready];
        events[ready].name = view_of(texts[2 * ready], nl);
        events[ready].traceparent = view_of(texts[2 * ready + 1], tl);
        napi_get_element(environment, argv[3], ready, &v);
        events[ready].data = payload_of_value(system, v);
        bool pending = false;
        napi_is_exception_pending(environment, &pending);
        if (pending) break;
    }
    uint32_t accepted = 0;
    while (accepted < ready) { /* a refused event is skipped (its session is gone) */
        accepted += ta_host_send_many(system->host, sessions + accepted, events + accepted,
                                      ready - accepted);
        if (accepted < ready) {
            payload_drop(system, events[accepted].data);
            events[accepted].data = 0;
            sessions[accepted] = 0;
            accepted++;
        }
    }
    uint32_t delivered = 0;
    for (uint32_t i = 0; i < ready; i++)
        delivered += sessions[i] != 0;
    for (uint32_t i = 0; i < 2 * (n + 1); i++)
        free(texts[i]);
    free(texts);
    free(events);
    free(sessions);
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    if (pending) return NULL;
    return input_done(system, num(environment, delivered));
}

/* post(system, session, { name, data, wire, sendid, origin, origintype, raw, traceparent,
 * flags, token }): an event with every field of ta_event_in */
napi_value js_post(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    napi_value o = argv[2];
    size_t lengths[7] = {0};
    static const char *const names[] = {"name", "sendid",      "origin", "origintype",
                                        "raw",  "traceparent", "wire"};
    char *texts[7];
    for (int i = 0; i < 7; i++)
        texts[i] = property_string(environment, o, names[i], &lengths[i]);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = view_of(texts[0], lengths[0]);
    event.sendid = view_of(texts[1], lengths[1]);
    event.origin = view_of(texts[2], lengths[2]);
    event.origintype = view_of(texts[3], lengths[3]);
    event.raw = view_of(texts[4], lengths[4]);
    event.traceparent = view_of(texts[5], lengths[5]);
    event.flags = (uint32_t)property_double(environment, o, "flags");
    event.token = (uint32_t)property_double(environment, o, "token");
    event.data = texts[6] ? payload_of_wire(system, view_of(texts[6], lengths[6]))
                          : payload_of_value(system, property(environment, o, "data"));
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    ta_status status = pending
                           ? TA_ERROR_INVALID
                           : ta_host_send(system->host, get_session(environment, argv[1]), &event);
    if (status != TA_OK) payload_drop(system, event.data);
    for (int i = 0; i < 7; i++)
        free(texts[i]);
    if (pending) return NULL;
    return input_done(system, undef(environment));
}

/* deliver(system, peer, { target, name, data | wire, origin, origintype, sendid, msgid,
 * dedup, hops, traceparent }) -> outcome (TA_ROUTE_*), -1 if it failed */
napi_value js_deliver(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    napi_value o = argv[2];
    static const char *const names[] = {"target", "name",  "origin",      "origintype", "sendid",
                                        "msgid",  "dedup", "traceparent", "wire"};
    size_t lengths[9] = {0};
    char *texts[9];
    for (int i = 0; i < 9; i++)
        texts[i] = property_string(environment, o, names[i], &lengths[i]);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = view_of(texts[0], lengths[0]);
    envelope.name = view_of(texts[1], lengths[1]);
    envelope.origin = view_of(texts[2], lengths[2]);
    envelope.origintype = view_of(texts[3], lengths[3]);
    envelope.sendid = view_of(texts[4], lengths[4]);
    envelope.msgid = view_of(texts[5], lengths[5]);
    envelope.deduplication_key = view_of(texts[6], lengths[6]);
    envelope.traceparent = view_of(texts[7], lengths[7]);
    envelope.hops = (uint32_t)property_double(environment, o, "hops");
    envelope.data = texts[8] ? payload_of_wire(system, view_of(texts[8], lengths[8]))
                             : payload_of_value(system, property(environment, o, "data"));
    envelope.datamodel = 0;
    uint32_t outcome = 0;
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    ta_host_advance_time(system->host, (ta_time)system_now(system));
    ta_status status =
        pending ? TA_ERROR_INVALID
                : ta_host_deliver(system->host, get_u32(environment, argv[1]), &envelope, &outcome);
    if (status != TA_OK) payload_drop(system, envelope.data);
    for (int i = 0; i < 9; i++)
        free(texts[i]);
    if (pending) return NULL;
    return input_done(system, num(environment, status == TA_OK ? (double)outcome : -1));
}

/* pump(system, fired) -> run code: run what is due (after a spawn, boot or adopt;
 * when the caller's clock moved); `fired`: callbacks.schedule's timer went off */
napi_value js_pump(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    bool fired = false;
    if (argc > 1) napi_get_value_bool(environment, argv[1], &fired);
    if (fired) system->armed = -3; /* the timer is no longer armed */
    double code = pump(system);
    return code == code ? num(environment, code) : NULL;
}

/* now(system) -> the system's time (ms) */
napi_value js_now(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    return system ? num(environment, system_now(system)) : NULL;
}

/* runs(system) -> { count, sum, min, max, bounds, buckets }: the pumps' durations (s) */
napi_value js_runs(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], o, bounds, buckets;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    napi_create_object(environment, &o);
    napi_create_array_with_length(environment, RUN_BUCKETS - 1, &bounds);
    napi_create_array_with_length(environment, RUN_BUCKETS, &buckets);
    for (uint32_t i = 0; i < RUN_BUCKETS; i++) {
        if (i < RUN_BUCKETS - 1)
            napi_set_element(environment, bounds, i, num(environment, run_bounds[i]));
        napi_set_element(environment, buckets, i, num(environment, (double)system->run_buckets[i]));
    }
    set(environment, o, "count", num(environment, (double)system->run_count));
    set(environment, o, "sum", num(environment, system->run_sum));
    set(environment, o, "min", num(environment, system->run_min));
    set(environment, o, "max", num(environment, system->run_max));
    set(environment, o, "bounds", bounds);
    set(environment, o, "buckets", buckets);
    return o;
}

napi_value js_terminate(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    return input_done(
        system,
        bool_value(environment,
                   ta_host_terminate(system->host, get_session(environment, argv[1])) == TA_OK));
}

int locate(nd_system *system, uint64_t session, ta_system **segment_system, uint32_t *segment,
           ta_actor_info *info) {
    uint32_t actor = 0;
    if (!session || ta_host_locate(system->host, session, segment, &actor) != TA_OK) return 0;
    *segment_system = ta_host_system(system->host, *segment);
    memset(info, 0, sizeof *info);
    info->size = sizeof *info;
    if (!*segment_system || ta_actor_info_get(*segment_system, actor, info) != TA_OK ||
        info->state == TA_ACTOR_FREE)
        return 0;
    return (int)actor;
}

/* inspect(system, session) -> { segment, chart, state (TA_ACTOR_*), states: [state
 * indices of its configuration] } | null */
napi_value js_inspect(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], o, states;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    ta_system *segment_system;
    uint32_t segment = 0, configuration[256];
    ta_actor_info actor_info;
    int actor =
        locate(system, get_session(environment, argv[1]), &segment_system, &segment, &actor_info);
    if (!actor) return null_value(environment);
    uint32_t n = ta_actor_config(segment_system, (uint32_t)actor, configuration, 256);
    napi_create_object(environment, &o);
    set(environment, o, "segment", num(environment, segment));
    set(environment, o, "chart", num(environment, actor_info.chart));
    set(environment, o, "state", num(environment, actor_info.state));
    set(environment, o, "parent", num(environment, actor_info.parent));
    napi_create_array_with_length(environment, n, &states);
    for (uint32_t i = 0; i < n && i < 256; i++)
        napi_set_element(environment, states, i, num(environment, configuration[i]));
    set(environment, o, "states", states);
    return o;
}

/* stateValue(system, session) -> its configuration as a state value (ta_host_state_value) */
napi_value js_state_value(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    uint64_t session = get_session(environment, argv[1]);
    char small[512];
    size_t length = ta_host_state_value(system->host, session, small, sizeof small);
    char *json = length < sizeof small ? small : (char *)malloc(length + 1);
    if (!json) abort();
    if (json != small) ta_host_state_value(system->host, session, json, length + 1);
    napi_value text = str_value(environment, view_of(length ? json : "{}", length ? length : 2));
    if (json != small) free(json);
    return call_global(environment, "JSON", "parse", 1, &text);
}

/* context(system, session) -> the session's variables (native: its scope) */
napi_value js_context(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    ta_system *segment_system;
    uint32_t segment = 0;
    ta_actor_info actor_info;
    if (!locate(system, get_session(environment, argv[1]), &segment_system, &segment,
                &actor_info) ||
        !actor_info.datamodel_instance)
        return undef(environment);
    if (system->native) {
        nd_instance *instance = instance_at(system, actor_info.datamodel_instance);
        return instance ? get_ref(environment, instance->scope) : undef(environment);
    }
    ta_host_segment_info segment_info;
    segment_info.size = sizeof segment_info;
    if (ta_host_segment_info_get(system->host, segment, &segment_info) != TA_OK)
        return undef(environment);
    return quickjs_variables(system, actor_info.datamodel_instance, segment_info.image,
                             actor_info.chart);
}

/* ---- inspection -------------------------------------------------------------------------------
 */

static void set_u64(napi_env environment, napi_value o, const char *name, uint64_t v) {
    set(environment, o, name, num(environment, (double)v));
}

/* stats(system) -> { actors, hibernated, runnable, segments, jobs, regionBytes, delivered,
 * routed, migrated, retired, sessions, payloads, charts: [...], segments: [...] } */
napi_value js_stats(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out, charts, list;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    ta_host_stats host;
    host.size = sizeof host;
    ta_host_stats_get(system->host, &host);
    napi_create_object(environment, &out);
    set_u64(environment, out, "actors", host.actors);
    set_u64(environment, out, "hibernated", host.hibernated);
    set_u64(environment, out, "runnable", host.runnable);
    set_u64(environment, out, "jobs", host.jobs);
    set_u64(environment, out, "regionBytes", host.region_bytes);
    set_u64(environment, out, "delivered", host.delivered);
    set_u64(environment, out, "routed", host.routed);
    set_u64(environment, out, "migrated", host.migrated);
    set_u64(environment, out, "retired", host.retired);
    set_u64(environment, out, "sessions",
            system->native ? system->live_instances : system->map_count);
    set_u64(environment, out, "payloads", system->live_payloads);
    if (system->quickjs)
        set_u64(environment, out, "heapBytes", ta_datamodel_ecmascript_heap_bytes(system->quickjs));
    uint32_t count = ta_host_chart_stats(system->host, NULL, 0);
    ta_chart_stats *chart_stats = (ta_chart_stats *)calloc(count + 1, sizeof *chart_stats);
    for (uint32_t i = 0; i < count; i++)
        chart_stats[i].size = sizeof chart_stats[i];
    count = ta_host_chart_stats(system->host, chart_stats, count);
    napi_create_array_with_length(environment, count, &charts);
    for (uint32_t i = 0; i < count; i++) {
        ta_chart_stats *c = &chart_stats[i];
        napi_value o;
        napi_create_object(environment, &o);
        set(environment, o, "name", str_value(environment, c->name));
        set_u64(environment, o, "actors", c->actors);
        set_u64(environment, o, "hibernated", c->hibernated);
        set_u64(environment, o, "spawned", c->spawned);
        set_u64(environment, o, "ended", c->ended);
        set_u64(environment, o, "turns", c->turns);
        set_u64(environment, o, "macrosteps", c->macrosteps);
        set_u64(environment, o, "fuel", c->fuel);
        set_u64(environment, o, "errorsExecution", c->errors_execution);
        set_u64(environment, o, "errorsCommunication", c->errors_communication);
        set_u64(environment, o, "errorsPlatform", c->errors_platform);
        set_u64(environment, o, "faults", c->faults);
        napi_set_element(environment, charts, i, o);
    }
    free(chart_stats);
    set(environment, out, "charts", charts);
    napi_create_array(environment, &list);
    uint32_t k = 0, segments = ta_host_segment_count(system->host);
    for (uint32_t segment = 0; segment < segments; segment++) {
        ta_host_segment_info segment_info;
        segment_info.size = sizeof segment_info;
        if (ta_host_segment_info_get(system->host, segment, &segment_info) != TA_OK ||
            !segment_info.live)
            continue;
        ta_system_stats stats;
        stats.size = sizeof stats;
        ta_system_stats_get(ta_host_system(system->host, segment), &stats);
        napi_value o;
        napi_create_object(environment, &o);
        set_u64(environment, o, "segment", segment);
        set_u64(environment, o, "actors", segment_info.actors);
        set_u64(environment, o, "current", segment_info.current);
        set_u64(environment, o, "actorsCapacity", stats.actors_capacity);
        set_u64(environment, o, "events", stats.events);
        set_u64(environment, o, "eventsCapacity", stats.events_capacity);
        set_u64(environment, o, "timers", stats.timers);
        set_u64(environment, o, "regionBytes", stats.region_bytes);
        set_u64(environment, o, "hibernated", stats.hibernated);
        napi_set_element(environment, list, k++, o);
    }
    set(environment, out, "segments", list);
    return out;
}

napi_value js_destroy(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *p = NULL;
    if (!argc || napi_get_value_external(environment, argv[0], &p) != napi_ok || !p)
        return undef(environment);
    nd_system *system = (nd_system *)p;
    if (system->running) { /* from an effect handler: the host goes when the pump ends */
        system->stopping = 1;
        return undef(environment);
    }
    system_destroy(environment, system);
    return undef(environment);
}
/* an array of strings as ta_string_view views (free with strs_free) */
ta_string_view *get_strs(napi_env environment, napi_value arr, uint32_t *n) {
    bool is = false;
    *n = 0;
    if (napi_is_array(environment, arr, &is) != napi_ok || !is) return NULL;
    napi_get_array_length(environment, arr, n);
    ta_string_view *out = (ta_string_view *)calloc(*n ? *n : 1, sizeof *out);
    for (uint32_t i = 0; out && i < *n; i++) {
        napi_value v;
        size_t l = 0;
        napi_get_element(environment, arr, i, &v);
        char *s = get_string(environment, v, &l);
        out[i] = (ta_string_view){s ? s : "", s ? (uint32_t)l : 0};
    }
    return out;
}
