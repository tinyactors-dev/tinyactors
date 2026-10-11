/* trace.c — the trace encoder (ta_otlp), its policy, and decoding traces (binding.h). */
#include "binding.h"

/* ---- decoding: OTLP traces to plain objects (core.mjs shapes them) ------------------------- */

napi_value decoded_id(napi_env environment, uint64_t high, uint64_t low, int wide) {
    char text[TA_TRACE_ID_TEXT_LENGTH];
    if (wide) {
        ta_trace_id_format((ta_trace_id){high, low}, text, sizeof text);
        return str_value(environment, (ta_string_view){text, TA_TRACE_ID_TEXT_LENGTH});
    }
    ta_span_id_format(low, text, sizeof text);
    return str_value(environment, (ta_string_view){text, TA_SPAN_ID_TEXT_LENGTH});
}

static napi_value decoded_u64(napi_env environment, uint64_t value) {
    char text[24];
    int n = snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return str_value(environment, (ta_string_view){text, (uint32_t)n});
}

/* a value: string, boolean, number, { int: "decimal" } beyond 2^53, array,
 * { list: [[k, v]] }, { bytes: Uint8Array } */
static napi_value decoded_value(napi_env environment, const ta_otlp_value *value) {
    napi_value v, wrapper;
    switch (value->kind) {
    case TA_OTLP_VALUE_STRING:
        return str_value(environment, value->bytes);
    case TA_OTLP_VALUE_BOOL:
        return bool_value(environment, value->integer != 0);
    case TA_OTLP_VALUE_INT:
        if (value->integer >= -9007199254740991LL && value->integer <= 9007199254740991LL)
            return num(environment, (double)value->integer);
        napi_create_object(environment, &wrapper);
        {
            char text[24];
            int n = snprintf(text, sizeof text, "%lld", (long long)value->integer);
            napi_set_named_property(environment, wrapper, "int",
                                    str_value(environment, (ta_string_view){text, (uint32_t)n}));
        }
        return wrapper;
    case TA_OTLP_VALUE_DOUBLE:
        return num(environment, value->number);
    case TA_OTLP_VALUE_ARRAY: {
        uint32_t at = 0, i = 0;
        ta_otlp_value item;
        napi_create_array(environment, &v);
        while (ta_otlp_next_value(value->items, &at, &item) == 1)
            napi_set_element(environment, v, i++, decoded_value(environment, &item));
        return v;
    }
    case TA_OTLP_VALUE_LIST:
        napi_create_object(environment, &wrapper);
        napi_set_named_property(environment, wrapper, "list",
                                decoded_attributes(environment, value->items));
        return wrapper;
    case TA_OTLP_VALUE_BYTES:
        napi_create_object(environment, &wrapper);
        napi_set_named_property(
            environment, wrapper, "bytes",
            bytes_value(environment, value->bytes.pointer, value->bytes.length));
        return wrapper;
    default:
        return null_value(environment);
    }
}

/* attributes as [[key, value], ...] */
napi_value decoded_attributes(napi_env environment, ta_otlp_fields fields) {
    napi_value list;
    napi_create_array(environment, &list);
    uint32_t at = 0, i = 0;
    ta_string_view key;
    ta_otlp_value value;
    while (ta_otlp_next_attribute(fields, &at, &key, &value) == 1) {
        napi_value pair;
        napi_create_array_with_length(environment, 2, &pair);
        napi_set_element(environment, pair, 0, str_value(environment, key));
        napi_set_element(environment, pair, 1, decoded_value(environment, &value));
        napi_set_element(environment, list, i++, pair);
    }
    return list;
}

/* decodeTraces(bytes) -> [span], or throws on malformed bytes */
napi_value js_decode_traces(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], spans;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *bytes = NULL;
    size_t length = 0;
    if (argc < 1 || !bytes_of(environment, argv[0], &bytes, &length)) {
        napi_throw_type_error(environment, NULL, "decodeTraces(bytes: Uint8Array)");
        return NULL;
    }
    napi_create_array(environment, &spans);
    ta_otlp_cursor cursor;
    memset(&cursor, 0, sizeof cursor);
    ta_otlp_span span;
    int32_t got;
    uint32_t i = 0;
    while ((got = ta_otlp_next_span(bytes, length, &cursor, &span)) == 1) {
        napi_value o, events, links;
        napi_create_object(environment, &o);
        napi_set_named_property(environment, o, "traceID",
                                decoded_id(environment, span.trace.high, span.trace.low, 1));
        napi_set_named_property(environment, o, "spanID", decoded_id(environment, 0, span.span, 0));
        if (span.parent)
            napi_set_named_property(environment, o, "parentSpanID",
                                    decoded_id(environment, 0, span.parent, 0));
        napi_set_named_property(environment, o, "name", str_value(environment, span.name));
        napi_set_named_property(environment, o, "kind", num(environment, span.kind));
        napi_set_named_property(environment, o, "start", decoded_u64(environment, span.start));
        napi_set_named_property(environment, o, "end", decoded_u64(environment, span.end));
        napi_set_named_property(environment, o, "status", num(environment, span.status));
        if (span.status_message.length)
            napi_set_named_property(environment, o, "statusMessage",
                                    str_value(environment, span.status_message));
        napi_set_named_property(environment, o, "attributes",
                                decoded_attributes(environment, span.attributes));
        napi_create_array(environment, &events);
        uint32_t at = 0, k = 0;
        ta_otlp_event event;
        while (ta_otlp_next_event(span.events, &at, &event) == 1) {
            napi_value e;
            napi_create_object(environment, &e);
            napi_set_named_property(environment, e, "name", str_value(environment, event.name));
            napi_set_named_property(environment, e, "time", decoded_u64(environment, event.time));
            napi_set_named_property(environment, e, "attributes",
                                    decoded_attributes(environment, event.attributes));
            napi_set_element(environment, events, k++, e);
        }
        napi_set_named_property(environment, o, "events", events);
        napi_create_array(environment, &links);
        at = 0;
        k = 0;
        ta_otlp_link link;
        while (ta_otlp_next_link(span.links, &at, &link) == 1) {
            napi_value l;
            napi_create_object(environment, &l);
            napi_set_named_property(environment, l, "traceID",
                                    decoded_id(environment, link.trace.high, link.trace.low, 1));
            napi_set_named_property(environment, l, "spanID",
                                    decoded_id(environment, 0, link.span, 0));
            napi_set_named_property(environment, l, "attributes",
                                    decoded_attributes(environment, link.attributes));
            napi_set_element(environment, links, k++, l);
        }
        napi_set_named_property(environment, o, "links", links);
        napi_set_named_property(environment, o, "resource",
                                decoded_attributes(environment, span.resource));
        napi_set_named_property(environment, o, "scopeName",
                                str_value(environment, span.scope_name));
        napi_set_named_property(environment, o, "scopeVersion",
                                str_value(environment, span.scope_version));
        napi_set_named_property(environment, o, "scopeAttributes",
                                decoded_attributes(environment, span.scope_attributes));
        napi_set_named_property(environment, o, "flags", num(environment, span.flags));
        napi_set_element(environment, spans, i++, o);
    }
    if (got < 0) {
        napi_throw_error(environment, NULL, "decodeTraces: malformed OTLP protobuf");
        return NULL;
    }
    return spans;
}

uint64_t trace_wall(nd_system *host) {
    if (!host->clock) return 0;
    napi_value clock = get_ref(host->environment, host->clock), global, r;
    napi_get_global(host->environment, &global);
    double ms = 0;
    if (napi_call_function(host->environment, global, clock, 0, NULL, &r) == napi_ok)
        napi_get_value_double(host->environment, r, &ms);
    return ms > 0 ? (uint64_t)(ms * 1e6) : 0;
}

uint64_t trace_fuel(nd_system *host) {
    ta_host_stats stats;
    stats.size = sizeof stats;
    ta_host_stats_get(host->host, &stats);
    return stats.fuel_clock;
}

/* Grow a turn buffer to hold `more` bytes more; 0 if refused. */
static int trace_room(unsigned char **buffer, size_t *length, size_t *capacity, size_t more) {
    if (*length + more <= *capacity) return 1;
    size_t c = *capacity ? *capacity : 65536;
    while (c < *length + more)
        c *= 2;
    unsigned char *grown = (unsigned char *)realloc(*buffer, c);
    if (!grown) return 0;
    *buffer = grown;
    *capacity = c;
    return 1;
}

/* The staged requests, onto this turn's. */
void trace_put(nd_system *host) {
    size_t size = ta_otlp_traces_size(host->otlp);
    if (size && trace_room(&host->traces, &host->traces_length, &host->traces_capacity, size))
        host->traces_length +=
            ta_otlp_put_traces(host->otlp, host->traces + host->traces_length, size);
    size = ta_otlp_logs_size(host->otlp);
    if (size && trace_room(&host->logs, &host->logs_length, &host->logs_capacity, size))
        host->logs_length += ta_otlp_put_logs(host->otlp, host->logs + host->logs_length, size);
}

/* A bigger encoder, its open spans and staging moved along; 0 if refused. */
static int trace_grow(nd_system *host) {
    ta_otlp_stats stats;
    stats.size = sizeof stats;
    ta_otlp_stats_get(host->otlp, &stats);
    ta_otlp_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.open_spans = stats.open_capacity * 2;
    config.chunks = stats.chunks_capacity * 2;
    config.staging_bytes = stats.staging_capacity;
    config.log_bytes = stats.log_capacity;
    size_t length = 0;
    if (ta_otlp_layout(&config, &length) != TA_OK) return 0;
    void *memory = malloc(length);
    ta_otlp *moved = NULL;
    if (!memory || ta_otlp_relocate(host->otlp, memory, length, &config, &moved) != TA_OK) {
        free(memory);
        return 0;
    }
    free(host->otlp);
    host->otlp = moved;
    host->otlp_length = length;
    return 1;
}

/* Read every segment's records into the encoder (or let them go without one). */
void trace_drain(nd_system *host) {
    ta_datamodel_binding *datamodels = &host->binding;
    uint32_t segments = ta_host_segment_count(host->host);
    for (uint32_t k = 0; k < segments; k++) {
        ta_system *system = ta_host_system(host->host, k);
        if (!system) continue;
        ta_host_segment_info info;
        info.size = sizeof info;
        ta_host_segment_info_get(host->host, k, &info);
        ta_trace_view view;
        while (ta_system_trace(system, &view)) {
            if (host->otlp) {
                ta_otlp_input input;
                memset(&input, 0, sizeof input);
                input.size = sizeof input;
                input.source = k;
                input.view = view;
                input.image = info.image;
                input.datamodels = datamodels;
                input.datamodel_count = 1;
                uint32_t cursor = 0, status;
                while ((status = ta_otlp_read(host->otlp, &input, &cursor)) != TA_OTLP_READ) {
                    if (status == TA_OTLP_FLUSH)
                        trace_put(host);
                    else if (!trace_grow(host))
                        break; /* no memory: these records are lost */
                }
            }
            ta_system_trace_ack(system, view.length);
        }
    }
}

void attributes_of(napi_env environment, napi_value pairs, js_attributes *out) {
    bool array = false;
    napi_is_array(environment, pairs, &array);
    uint32_t n = 0;
    if (array) napi_get_array_length(environment, pairs, &n);
    out->count = n;
    out->list = (ta_otlp_attribute *)calloc(n ? n : 1, sizeof *out->list);
    out->texts = (char **)calloc(2 * (n ? n : 1), sizeof(char *));
    for (uint32_t i = 0; i < n; i++) {
        napi_value pair, key, value;
        napi_get_element(environment, pairs, i, &pair);
        napi_get_element(environment, pair, 0, &key);
        napi_get_element(environment, pair, 1, &value);
        size_t kl = 0, vl = 0;
        out->texts[2 * i] = get_string(environment, key, &kl);
        out->texts[2 * i + 1] = get_string(environment, value, &vl);
        out->list[i].key =
            (ta_string_view){out->texts[2 * i] ? out->texts[2 * i] : "", (uint32_t)kl};
        out->list[i].value =
            (ta_string_view){out->texts[2 * i + 1] ? out->texts[2 * i + 1] : "", (uint32_t)vl};
    }
}

void attributes_free(js_attributes *attributes) {
    for (uint32_t i = 0; i < 2 * attributes->count; i++)
        free(attributes->texts[i]);
    free(attributes->texts);
    free(attributes->list);
}

/* An encoder's resource from [[key, value], ...]. */
void otlp_resource_set(napi_env environment, ta_otlp *otlp, napi_value pairs) {
    js_attributes attributes;
    attributes_of(environment, pairs, &attributes);
    ta_otlp_set_resource(otlp, attributes.list, attributes.count);
    attributes_free(&attributes);
}

/* A view of a string property (an empty one without it); the text is the
 * caller's to free. */
static ta_string_view named_text(napi_env environment, napi_value object, const char *name,
                                 char **text) {
    napi_value v;
    napi_valuetype t = napi_undefined;
    size_t length = 0;
    *text = NULL;
    if (napi_get_named_property(environment, object, name, &v) == napi_ok)
        napi_typeof(environment, v, &t);
    if (t == napi_string) *text = get_string(environment, v, &length);
    return (ta_string_view){*text ? *text : "", (uint32_t)length};
}

/* traceConfigure(host, { enabled, classes: [8 kind masks], sampleParts, sampleWhole,
 * rules: [{ at, chart, actor, event, session (decimal), from, sample, parts,
 * whole, detail (-1: keep), perSecond, burst }], flags, valueLimit, resource:
 * [[key, value], ...] }): what core writes (docs/adr/0007), and the encoder
 * that reads it (none when not enabled: core then writes nothing, contexts
 * still travel) */
napi_value js_trace_configure(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], v;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    bool enabled = false;
    if (napi_get_named_property(environment, argv[1], "enabled", &v) == napi_ok)
        napi_get_value_bool(environment, v, &enabled);
    uint32_t parts = 1, whole = 1, flags = 0, value_limit = 0;
    if (napi_get_named_property(environment, argv[1], "sampleParts", &v) == napi_ok)
        parts = get_u32(environment, v);
    if (napi_get_named_property(environment, argv[1], "sampleWhole", &v) == napi_ok)
        whole = get_u32(environment, v);
    if (napi_get_named_property(environment, argv[1], "flags", &v) == napi_ok)
        flags = get_u32(environment, v);
    if (napi_get_named_property(environment, argv[1], "valueLimit", &v) == napi_ok)
        value_limit = get_u32(environment, v);
    if (!whole) parts = 0, whole = 1; /* nothing sampled */
    /* no destination: no records (the dropped kind alone), contexts still flow */
    ta_trace_policy policy;
    ta_trace_policy_default(&policy);
    policy.sample_parts = parts;
    policy.sample_whole = whole;
    napi_value classes;
    bool is_array = false;
    if (napi_get_named_property(environment, argv[1], "classes", &classes) == napi_ok)
        napi_is_array(environment, classes, &is_array);
    for (uint32_t i = 0; i < TA_TRACE_DETAILS; i++) {
        uint32_t kinds = 0;
        if (enabled && is_array && napi_get_element(environment, classes, i, &v) == napi_ok)
            kinds = get_u32(environment, v);
        policy.kinds[i] = kinds ? kinds : TA_TRACE_BIT(TA_TRACE_DROPPED);
    }
    napi_value rules_js;
    uint32_t rule_count = named_length(environment, argv[1], "rules", &rules_js);
    ta_trace_rule *rules = (ta_trace_rule *)calloc(rule_count ? rule_count : 1, sizeof *rules);
    char **texts = (char **)calloc(3 * (rule_count ? rule_count : 1), sizeof(char *));
    for (uint32_t i = 0; i < rule_count; i++) {
        napi_value rule;
        napi_get_element(environment, rules_js, i, &rule);
        ta_trace_rule *r = &rules[i];
        r->size = sizeof *r;
        r->at = (uint32_t)named_double(environment, rule, "at");
        r->chart = named_text(environment, rule, "chart", &texts[3 * i]);
        r->actor = named_text(environment, rule, "actor", &texts[3 * i + 1]);
        r->event = named_text(environment, rule, "event", &texts[3 * i + 2]);
        if (napi_get_named_property(environment, rule, "session", &v) == napi_ok)
            r->session = decimal_of(environment, v);
        r->from = (uint32_t)named_double(environment, rule, "from");
        r->sample = (uint32_t)named_double(environment, rule, "sample");
        r->sample_parts = (uint32_t)named_double(environment, rule, "parts");
        r->sample_whole = (uint32_t)named_double(environment, rule, "whole");
        double detail = named_double(environment, rule, "detail");
        r->detail = detail < 0 ? TA_NIL : (uint32_t)detail;
        r->per_second = (uint32_t)named_double(environment, rule, "perSecond");
        r->burst = (uint32_t)named_double(environment, rule, "burst");
    }
    policy.rules = rules;
    policy.rule_count = rule_count;
    ta_status status = ta_host_trace_set(host->host, &policy);
    for (uint32_t i = 0; i < 3 * rule_count; i++)
        free(texts[i]);
    free(texts);
    free(rules);
    if (status == TA_ERROR_NO_MEMORY || status == TA_ERROR_INVALID) {
        napi_throw_error(environment, NULL,
                         status == TA_ERROR_NO_MEMORY
                             ? "traces: more rules than a system holds (64), or no memory"
                             : "traces: a rule is malformed, or names more than 16 actors");
        return NULL;
    }
    if (!enabled) {
        free(host->otlp);
        host->otlp = NULL;
        return undef(environment);
    }
    if (!host->otlp) {
        ta_otlp_config config;
        memset(&config, 0, sizeof config);
        config.size = sizeof config;
        config.flags = flags;
        config.value_limit = value_limit;
        size_t length = 0;
        ta_otlp_layout(&config, &length);
        void *memory = malloc(length);
        if (!memory || ta_otlp_init(memory, length, &config, &host->otlp) != TA_OK) {
            free(memory);
            host->otlp = NULL;
            napi_throw_error(environment, NULL, "cannot create the trace encoder");
            return NULL;
        }
        host->otlp_length = length;
    }
    ta_otlp_set_flags(host->otlp, flags);
    if (napi_get_named_property(environment, argv[1], "resource", &v) == napi_ok)
        otlp_resource_set(environment, host->otlp, v);
    return undef(environment);
}

/* traceEnd(host) -> { traces, logs }: every open span ends (incomplete), for
 * a system going away */
napi_value js_trace_end(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    napi_create_object(environment, &out);
    if (!host->otlp) return out;
    trace_drain(host);
    for (int i = 0; i < 64; i++) {
        ta_otlp_stats stats;
        stats.size = sizeof stats;
        ta_otlp_end_all(host->otlp, trace_wall(host));
        trace_put(host);
        ta_otlp_stats_get(host->otlp, &stats);
        if (!stats.open) break;
    }
    if (host->traces_length)
        napi_set_named_property(environment, out, "traces",
                                bytes_value(environment, host->traces, host->traces_length));
    if (host->logs_length)
        napi_set_named_property(environment, out, "traceLogs",
                                bytes_value(environment, host->logs, host->logs_length));
    host->traces_length = host->logs_length = 0;
    return out;
}
