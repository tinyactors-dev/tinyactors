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

/* ---- the trace options of the API, as the host takes them (ta_host_trace_options) */

typedef struct name_bit {
    const char *name;
    uint32_t bit;
} name_bit;
static const name_bit rule_at[] = {{"start", TA_TRACE_AT_START}, {"take", TA_TRACE_AT_TAKE}};
static const name_bit rule_from[] = {
    {"host", TA_TRACE_FROM_HOST},       {"actor", TA_TRACE_FROM_ACTOR},
    {"timer", TA_TRACE_FROM_TIMER},     {"spawn", TA_TRACE_FROM_SPAWN},
    {"sampled", TA_TRACE_FROM_SAMPLED}, {"unsampled", TA_TRACE_FROM_UNSAMPLED}};

#define SAMPLE_WHOLE 1000000u
#define WATCH_MS     (15.0 * 60 * 1000) /* a watch ends by itself after this, unless told */

static int type_error(napi_env environment, const char *message) {
    napi_throw_type_error(environment, NULL, message);
    return 0;
}

/* names (one, or an array of them) as bits of `table`; 0 when one is not in it (thrown) */
static int bits_of(napi_env environment, napi_value v, const name_bit *table, uint32_t count,
                   const char *what, uint32_t *bits) {
    *bits = 0;
    if (type_of(environment, v) == napi_undefined) return 1;
    bool array = false;
    napi_is_array(environment, v, &array);
    uint32_t n = array ? array_length(environment, v) : 1;
    for (uint32_t i = 0; i < n; i++) {
        napi_value name = v;
        if (array) napi_get_element(environment, v, i, &name);
        size_t length = 0;
        char *text = type_of(environment, name) == napi_string
                         ? get_string(environment, name, &length)
                         : NULL;
        uint32_t k = 0;
        while (text && k < count && strcmp(text, table[k].name) != 0)
            k++;
        free(text);
        if (!text || k == count) {
            char message[160];
            snprintf(message, sizeof message, "%s: one of %s%s%s%s%s%s", what, table[0].name,
                     count > 1 ? ", " : "", count > 1 ? table[1].name : "", count > 2 ? ", " : "",
                     count > 2 ? table[2].name : "", count > 3 ? ", ..." : "");
            return type_error(environment, message);
        }
        *bits |= table[k].bit;
    }
    return 1;
}

/* an array of kind names as a mask (ta_trace_kind_by_name) */
static int kinds_mask(napi_env environment, napi_value names, uint32_t *mask) {
    *mask = 0;
    uint32_t n = array_length(environment, names);
    for (uint32_t i = 0; i < n; i++) {
        napi_value name;
        napi_get_element(environment, names, i, &name);
        size_t length = 0;
        char *text = type_of(environment, name) == napi_string
                         ? get_string(environment, name, &length)
                         : NULL;
        uint32_t kind = text ? ta_trace_kind_by_name(view_of(text, length)) : 0;
        free(text);
        if (!kind) return type_error(environment, "not a trace kind (see TRACE_KINDS)");
        *mask |= TA_TRACE_BIT(kind);
    }
    return 1;
}

/* a detail's kinds: "summary" | "decisions" | "full" | "off" | { kinds, values }; full
 * when undefined; `top`: the system's own detail, where "off" is no detail */
static int detail_kinds(napi_env environment, napi_value detail, napi_value kinds, int values,
                        int top, uint32_t *out) {
    bool array = false;
    if (kinds) napi_is_array(environment, kinds, &array);
    napi_valuetype t = type_of(environment, detail);
    if (array) {
        if (!kinds_mask(environment, kinds, out)) return 0;
    } else if (t == napi_undefined) {
        *out = TA_TRACE_FULL;
    } else if (t == napi_string) {
        size_t length = 0;
        char *text = get_string(environment, detail, &length);
        ta_status status = ta_trace_detail_by_name(view_of(text, length), out);
        int no_detail = text && !strcmp(text, "off");
        free(text);
        if (status != TA_OK || (top && no_detail))
            return type_error(environment,
                              top ? "detail: \"summary\", \"decisions\" or \"full\""
                                  : "detail: \"summary\", \"decisions\", \"full\" or \"off\"");
    } else if (t == napi_object) {
        napi_value inner = property(environment, detail, "kinds");
        napi_is_array(environment, inner, &array);
        if (!array || !kinds_mask(environment, inner, out))
            return array ? 0
                         : type_error(environment, "detail: \"summary\", \"decisions\", \"full\", "
                                                   "\"off\" or { kinds, values }");
        values = values || property_bool(environment, detail, "values");
    } else {
        return type_error(
            environment,
            "detail: \"summary\", \"decisions\", \"full\", \"off\" or { kinds, values }");
    }
    if (values) *out |= TA_TRACE_VALUES;
    return 1;
}

/* a ratio from 0 to 1 as parts of SAMPLE_WHOLE (all of them when undefined) */
static int sample_parts(napi_env environment, napi_value v, uint32_t *parts) {
    *parts = SAMPLE_WHOLE;
    if (type_of(environment, v) == napi_undefined) return 1;
    double ratio = type_of(environment, v) == napi_number ? get_double(environment, v) : -1;
    if (!(ratio >= 0 && ratio <= 1)) {
        napi_throw_range_error(environment, NULL, "sample: a ratio from 0 to 1");
        return 0;
    }
    *parts = (uint32_t)(ratio * SAMPLE_WHOLE + 0.5);
    return 1;
}

/* a rule of the API ({ at, from, chart, actor, event, session, sample, detail, values,
 * perSecond, burst }); its strings are texts[0..2], the caller's to free */
static int rule_of(napi_env environment, napi_value rule, ta_trace_rule *r, char **texts) {
    memset(r, 0, sizeof *r);
    r->size = sizeof *r;
    if (type_of(environment, rule) != napi_object)
        return type_error(environment, "a trace rule is an object");
    napi_value sample = property(environment, rule, "sample");
    napi_valuetype sample_type = type_of(environment, sample);
    if (sample_type == napi_boolean)
        r->sample = property_bool(environment, rule, "sample") ? TA_TRACE_SAMPLE : TA_TRACE_DROP;
    else if (sample_type != napi_undefined) {
        r->sample = TA_TRACE_RATIO;
        r->sample_whole = SAMPLE_WHOLE;
        if (!sample_parts(environment, sample, &r->sample_parts)) return 0;
    }
    r->chart = named_text(environment, rule, "chart", &texts[0]);
    r->actor = named_text(environment, rule, "actor", &texts[1]);
    r->event = named_text(environment, rule, "event", &texts[2]);
    napi_value session = property(environment, rule, "session");
    if (type_of(environment, session) != napi_undefined)
        r->session = decimal_of(environment, session);
    if (!bits_of(environment, property(environment, rule, "at"), rule_at, 2, "at", &r->at) ||
        !bits_of(environment, property(environment, rule, "from"), rule_from, 6, "from", &r->from))
        return 0;
    napi_value detail = property(environment, rule, "detail");
    r->detail = TA_NIL;
    if (type_of(environment, detail) != napi_undefined &&
        !detail_kinds(environment, detail, NULL, property_bool(environment, rule, "values"), 0,
                      &r->detail))
        return 0;
    r->per_second = (uint32_t)named_double(environment, rule, "perSecond");
    r->burst = (uint32_t)named_double(environment, rule, "burst");
    return 1;
}

static void throw_trace_status(napi_env environment, ta_status status) {
    napi_throw_error(environment, NULL,
                     status == TA_ERROR_NO_MEMORY
                         ? "traces: more rules than a system holds (64), more than 6 different "
                           "details in rules and watches, or no memory"
                         : "traces: a rule is malformed, or names more than 16 actors");
}

/* the encoder's flags with what the watches need (their values) */
static void encoder_flags(nd_system *system) {
    if (system->otlp)
        ta_otlp_set_flags(system->otlp,
                          system->otlp_flags | (system->watch_values ? TA_OTLP_VALUES : 0));
}

/* traceConfigure(system, { enabled, detail, kinds, values, sample, rules, parent, logs,
 * spanMetrics, valueLimit, resource: [[key, value], ...] }): what the systems write
 * (ta_host_trace_set) and the encoder that reads it (none when not enabled: then no
 * records are written; traceparents still travel) */
napi_value js_trace_configure(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], v;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    napi_value o = argv[1];
    int enabled = property_bool(environment, o, "enabled"),
        values = property_bool(environment, o, "values");
    ta_host_trace_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.flags = enabled ? 0 : TA_HOST_TRACE_NO_RECORDS;
    options.sample_whole = SAMPLE_WHOLE;
    uint32_t flags = 0;
    if (!detail_kinds(environment, property(environment, o, "detail"),
                      property(environment, o, "kinds"), values, 1, &options.kinds) ||
        !sample_parts(environment, property(environment, o, "sample"), &options.sample_parts))
        return NULL;
    size_t pl = 0;
    char *parent = property_string(environment, o, "parent", &pl);
    if (parent && !strcmp(parent, "link")) flags |= TA_OTLP_LINK_PARENTS;
    int bad_parent = parent && strcmp(parent, "link") && strcmp(parent, "cause");
    free(parent);
    if (bad_parent) return type_error(environment, "parent: \"cause\" or \"link\""), NULL;
    if (values) flags |= TA_OTLP_VALUES;
    v = property(environment, o, "logs");
    if (type_of(environment, v) == napi_boolean && !property_bool(environment, o, "logs"))
        flags |= TA_OTLP_NO_LOGS;
    if (property_bool(environment, o, "spanMetrics")) flags |= TA_OTLP_SPAN_METRICS;
    napi_value rules_js;
    uint32_t count = named_length(environment, o, "rules", &rules_js);
    ta_trace_rule *rules = (ta_trace_rule *)calloc(count ? count : 1, sizeof *rules);
    char **texts = (char **)calloc(3 * (count ? count : 1), sizeof(char *));
    int ok = rules && texts;
    for (uint32_t i = 0; ok && i < count; i++) {
        napi_value rule;
        napi_get_element(environment, rules_js, i, &rule);
        ok = rule_of(environment, rule, &rules[i], &texts[3 * i]);
        if (ok && rules[i].detail != TA_NIL && (rules[i].detail & TA_TRACE_VALUES))
            flags |= TA_OTLP_VALUES;
    }
    ta_status status = TA_ERROR_INVALID;
    if (ok) {
        options.rules = rules;
        options.rule_count = count;
        status = ta_host_trace_set(host->host, &options);
    }
    for (uint32_t i = 0; texts && i < 3 * count; i++)
        free(texts[i]);
    free(texts);
    free(rules);
    if (!ok) return NULL;
    if (status != TA_OK) {
        throw_trace_status(environment, status);
        return NULL;
    }
    host->otlp_flags = flags;
    host->trace_sink = property_bool(environment, o, "sink");
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
        config.value_limit = (uint32_t)named_double(environment, o, "valueLimit");
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
    encoder_flags(host);
    if (napi_get_named_property(environment, o, "resource", &v) == napi_ok)
        otlp_resource_set(environment, host->otlp, v);
    return undef(environment);
}

/* watch(system, { sessions, names, charts, events, detail, values, perSecond, for: ms })
 * -> id: steps matching any of them are traced in full detail, sampled whatever
 * the ratio, with what they cause, until `for` ms of the system's time passed
 * (15 minutes; Infinity: until unwatch) */
napi_value js_watch(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    napi_value o = argv[1];
    uint32_t kinds = 0;
    napi_value detail = property(environment, o, "detail");
    if (type_of(environment, detail) == napi_undefined) kinds = TA_TRACE_FULL;
    int values = property_bool(environment, o, "values");
    if (type_of(environment, detail) != napi_undefined &&
        !detail_kinds(environment, detail, NULL, values, 0, &kinds))
        return NULL;
    if (values) kinds |= TA_TRACE_VALUES;
    static const char *const lists[] = {"sessions", "names", "charts", "events"};
    napi_value arrays[4];
    uint32_t counts[4], total = 0;
    for (int k = 0; k < 4; k++)
        total += counts[k] = named_length(environment, o, lists[k], &arrays[k]);
    if (!total)
        return type_error(environment,
                          "watchTraces: name actors, sessions, names, charts or events"),
               NULL;
    ta_trace_rule *rules = (ta_trace_rule *)calloc(total, sizeof *rules);
    char **texts = (char **)calloc(total, sizeof(char *));
    if (!rules || !texts) abort();
    uint32_t n = 0;
    for (int k = 0; k < 4; k++)
        for (uint32_t i = 0; i < counts[k]; i++, n++) {
            napi_value item;
            napi_get_element(environment, arrays[k], i, &item);
            ta_trace_rule *r = &rules[n];
            r->size = sizeof *r;
            r->at = TA_TRACE_AT_START | TA_TRACE_AT_TAKE;
            r->sample = TA_TRACE_SAMPLE;
            r->detail = kinds;
            r->per_second = (uint32_t)named_double(environment, o, "perSecond");
            size_t length = 0;
            if (k == 0) {
                r->session = decimal_of(environment, item);
                continue;
            }
            texts[n] = get_string(environment, item, &length);
            ta_string_view text = view_of(texts[n] ? texts[n] : "", length);
            if (k == 1)
                r->actor = text;
            else if (k == 2)
                r->chart = text;
            else
                r->event = text;
        }
    napi_value ms_value = property(environment, o, "for");
    double ms = type_of(environment, ms_value) == napi_number ? get_double(environment, ms_value)
                                                              : WATCH_MS;
    double now = system_now(host);
    ta_time until = ms > 0 && ms < 1e15 ? (ta_time)(now + ms) : UINT64_MAX;
    uint32_t id = 0;
    ta_status status = ta_host_trace_watch(host->host, rules, total, until, &id);
    for (uint32_t i = 0; i < total; i++)
        free(texts[i]);
    free(texts);
    free(rules);
    if (status != TA_OK) {
        throw_trace_status(environment, status);
        return NULL;
    }
    if (values) host->watch_values = 1;
    encoder_flags(host);
    return input_done(host, num(environment, id)); /* its end is a deadline now */
}

/* unwatch(system, id): the watch ends now */
napi_value js_unwatch(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    ta_host_trace_unwatch(host->host, get_u32(environment, argv[1]));
    return undef(environment);
}

/* the kinds' names, in core's order from 1 (ta_trace_kind_name) */
napi_value trace_kind_names(napi_env environment) {
    napi_value names;
    napi_create_array_with_length(environment, TA_TRACE_KIND_COUNT - 1, &names);
    for (uint32_t kind = 1; kind < TA_TRACE_KIND_COUNT; kind++)
        napi_set_element(environment, names, kind - 1,
                         str_value(environment, ta_trace_kind_name(kind)));
    napi_object_freeze(environment, names);
    return names;
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
    exporter_add(host, TA_OTLP_TRACES, host->traces, host->traces_length);
    exporter_add(host, TA_OTLP_LOGS, host->logs, host->logs_length);
    if (host->traces_length)
        napi_set_named_property(environment, out, "traces",
                                bytes_value(environment, host->traces, host->traces_length));
    if (host->logs_length)
        napi_set_named_property(environment, out, "traceLogs",
                                bytes_value(environment, host->logs, host->logs_length));
    host->traces_length = host->logs_length = 0;
    return out;
}
