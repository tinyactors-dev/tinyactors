/* addon.c — Node-API helpers, the export table and the constants (binding.h). */
#include "binding.h"

/* ---- small helpers ------------------------------------------------------------------- */

char *get_string(napi_env environment, napi_value v, size_t *length_out) {
    size_t length = 0;
    if (napi_get_value_string_utf8(environment, v, NULL, 0, &length) != napi_ok) return NULL;
    char *s = (char *)malloc(length + 1);
    if (!s) return NULL;
    napi_get_value_string_utf8(environment, v, s, length + 1, &length);
    if (length_out) *length_out = length;
    return s;
}

napi_value str_value(napi_env environment, ta_string_view s) {
    napi_value v;
    napi_create_string_utf8(environment, s.pointer ? s.pointer : "", s.length, &v);
    return v;
}

napi_value undef(napi_env environment) {
    napi_value u;
    napi_get_undefined(environment, &u);
    return u;
}

napi_value null_value(napi_env environment) {
    napi_value r;
    napi_get_null(environment, &r);
    return r;
}

napi_value bool_value(napi_env environment, int b) {
    napi_value r;
    napi_get_boolean(environment, b, &r);
    return r;
}

napi_value num(napi_env environment, double d) {
    napi_value v;
    napi_create_double(environment, d, &v);
    return v;
}

napi_valuetype type_of(napi_env environment, napi_value v) {
    napi_valuetype t = napi_undefined;
    if (v) napi_typeof(environment, v, &t);
    return t;
}

uint32_t get_u32(napi_env environment, napi_value v) {
    uint32_t x = 0;
    napi_get_value_uint32(environment, v, &x);
    return x;
}

double get_double(napi_env environment, napi_value v) {
    double d = 0;
    napi_get_value_double(environment, v, &d);
    return d;
}

/* Session ids cross as numbers: this host numbers sessions segment << 32 | n, below 2^53. */
uint64_t get_session(napi_env environment, napi_value v) {
    double d = get_double(environment, v);
    return d > 0 ? (uint64_t)d : 0;
}

/* References are boxed ({ v: value }): before Node-API 10 a reference can
 * only hold objects, functions and symbols, but values may be primitives. */
napi_ref make_ref(napi_env environment, napi_value v) {
    napi_value box;
    napi_ref r = NULL;
    if (napi_create_object(environment, &box) != napi_ok) return NULL;
    napi_set_named_property(environment, box, "v", v);
    napi_create_reference(environment, box, 1, &r);
    return r;
}

napi_value get_ref(napi_env environment, napi_ref r) {
    napi_value box = NULL, v = NULL;
    if (r && napi_get_reference_value(environment, r, &box) == napi_ok && box)
        napi_get_named_property(environment, box, "v", &v);
    return v ? v : undef(environment);
}

void set_ref(napi_env environment, napi_ref *r, napi_value v) {
    if (*r) napi_delete_reference(environment, *r);
    *r = v ? make_ref(environment, v) : NULL;
}

napi_value bytes_value(napi_env environment, const void *p, size_t n) {
    napi_value ab, u8;
    void *data = NULL;
    napi_create_arraybuffer(environment, n, &data, &ab);
    if (n) memcpy(data, p, n);
    napi_create_typedarray(environment, napi_uint8_array, n, ab, 0, &u8);
    return u8;
}

int bytes_of(napi_env environment, napi_value v, void **data, size_t *n) {
    napi_typedarray_type t;
    napi_value ab;
    size_t offset = 0;
    if (type_of(environment, v) != napi_object ||
        napi_get_typedarray_info(environment, v, &t, n, data, &ab, &offset) != napi_ok ||
        t != napi_uint8_array)
        return 0;
    return 1;
}

napi_value property(napi_env environment, napi_value o, const char *name) {
    napi_value v = NULL;
    if (type_of(environment, o) != napi_object ||
        napi_get_named_property(environment, o, name, &v) != napi_ok)
        return undef(environment);
    return v;
}

/* A string property's text (NULL when it is not a string); the caller frees it. */
char *property_string(napi_env environment, napi_value o, const char *name, size_t *n) {
    napi_value v = property(environment, o, name);
    *n = 0;
    return type_of(environment, v) == napi_string ? get_string(environment, v, n) : NULL;
}

double property_double(napi_env environment, napi_value o, const char *name) {
    napi_value v = property(environment, o, name);
    return type_of(environment, v) == napi_number ? get_double(environment, v) : 0;
}

int property_bool(napi_env environment, napi_value o, const char *name) {
    napi_value v = property(environment, o, name);
    bool b = false;
    if (type_of(environment, v) == napi_boolean) napi_get_value_bool(environment, v, &b);
    return b;
}

uint32_t array_length(napi_env environment, napi_value v) {
    bool is_array = false;
    uint32_t n = 0;
    if (v && napi_is_array(environment, v, &is_array) == napi_ok && is_array)
        napi_get_array_length(environment, v, &n);
    return n;
}

void clear_exception(napi_env environment) {
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    if (pending) {
        napi_value ignored;
        napi_get_and_clear_last_exception(environment, &ignored);
    }
}

/* Call a global's method, e.g. JSON.parse. */
napi_value call_global(napi_env environment, const char *object, const char *method, size_t argc,
                       napi_value *argv) {
    napi_value global, o, f, r = NULL;
    napi_get_global(environment, &global);
    napi_get_named_property(environment, global, object, &o);
    napi_get_named_property(environment, o, method, &f);
    if (napi_call_function(environment, o, f, argc, argv, &r) != napi_ok) return NULL;
    return r;
}

napi_value call(napi_env environment, napi_value f, size_t argc, napi_value *argv) {
    napi_value global, r = NULL;
    napi_get_global(environment, &global);
    if (napi_call_function(environment, global, f, argc, argv, &r) != napi_ok) return NULL;
    return r;
}

void set(napi_env environment, napi_value o, const char *name, napi_value v) {
    napi_set_named_property(environment, o, name, v);
}

int text_room(text *t, size_t more) {
    if (t->failed) return 0;
    if (t->length + more <= t->capacity) return 1;
    size_t c = t->capacity ? t->capacity : 256;
    while (c < t->length + more)
        c *= 2;
    char *grown = (char *)realloc(t->bytes, c);
    if (!grown) {
        t->failed = 1;
        return 0;
    }
    t->bytes = grown;
    t->capacity = c;
    return 1;
}

void text_put(text *t, const char *p, size_t n) {
    if (n && text_room(t, n)) {
        memcpy(t->bytes + t->length, p, n);
        t->length += n;
    }
}

void text_char(text *t, char c) {
    text_put(t, &c, 1);
}

void text_string(text *t, const char *p, size_t n) { /* a JSON string */
    static const char hex[] = "0123456789abcdef";
    text_char(t, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '"' || c == '\\') {
            text_char(t, '\\');
            text_char(t, (char)c);
        } else if (c < 0x20) {
            char escape[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            text_put(t, escape, 6);
        } else {
            text_char(t, (char)c);
        }
    }
    text_char(t, '"');
}

ta_string_view view_of(const char *p, size_t n) {
    return (ta_string_view){p ? p : "", (uint32_t)n};
}

/* constants: the numbers of the public headers core.mjs needs (trace kinds and
 * presets, flags), so no JavaScript twin keeps them */
static void constant(napi_env environment, napi_value o, const char *name, double value) {
    set(environment, o, name, num(environment, value));
}

static napi_value constants_value(napi_env environment) {
    napi_value o;
    napi_create_object(environment, &o);
    constant(environment, o, "TRACE_LIFECYCLE", TA_TRACE_LIFECYCLE);
    constant(environment, o, "TRACE_SUMMARY", TA_TRACE_SUMMARY);
    constant(environment, o, "TRACE_DECISIONS", TA_TRACE_DECISIONS);
    constant(environment, o, "TRACE_FULL", TA_TRACE_FULL);
    constant(environment, o, "TRACE_VALUES", TA_TRACE_VALUES);
    constant(environment, o, "TRACE_DETAILS", TA_TRACE_DETAILS);
    constant(environment, o, "TRACE_KIND_COUNT", TA_TRACE_KIND_COUNT);
    constant(environment, o, "TRACE_AT_START", TA_TRACE_AT_START);
    constant(environment, o, "TRACE_AT_TAKE", TA_TRACE_AT_TAKE);
    constant(environment, o, "TRACE_FROM_HOST", TA_TRACE_FROM_HOST);
    constant(environment, o, "TRACE_FROM_ACTOR", TA_TRACE_FROM_ACTOR);
    constant(environment, o, "TRACE_FROM_TIMER", TA_TRACE_FROM_TIMER);
    constant(environment, o, "TRACE_FROM_SPAWN", TA_TRACE_FROM_SPAWN);
    constant(environment, o, "TRACE_FROM_SAMPLED", TA_TRACE_FROM_SAMPLED);
    constant(environment, o, "TRACE_FROM_UNSAMPLED", TA_TRACE_FROM_UNSAMPLED);
    constant(environment, o, "TRACE_KEEP", TA_TRACE_KEEP);
    constant(environment, o, "TRACE_SAMPLE", TA_TRACE_SAMPLE);
    constant(environment, o, "TRACE_DROP", TA_TRACE_DROP);
    constant(environment, o, "TRACE_RATIO", TA_TRACE_RATIO);
    constant(environment, o, "OTLP_LINK_PARENTS", TA_OTLP_LINK_PARENTS);
    constant(environment, o, "OTLP_VALUES", TA_OTLP_VALUES);
    constant(environment, o, "OTLP_NO_LOGS", TA_OTLP_NO_LOGS);
    constant(environment, o, "OTLP_SPAN_METRICS", TA_OTLP_SPAN_METRICS);
    constant(environment, o, "OTLP_METRIC_SUM", TA_OTLP_METRIC_SUM);
    constant(environment, o, "OTLP_METRIC_GAUGE", TA_OTLP_METRIC_GAUGE);
    constant(environment, o, "OTLP_METRIC_HISTOGRAM", TA_OTLP_METRIC_HISTOGRAM);
    constant(environment, o, "PEER_TRUSTED", TA_PEER_TRUSTED);
    constant(environment, o, "PEER_QUEUE", TA_PEER_QUEUE);
    constant(environment, o, "PEER_CONNECTING", TA_PEER_CONNECTING);
    constant(environment, o, "PEER_OPEN", TA_PEER_OPEN);
    constant(environment, o, "PEER_RECONNECTING", TA_PEER_RECONNECTING);
    constant(environment, o, "PEER_CLOSED", TA_PEER_CLOSED);
    constant(environment, o, "ROUTE_DELIVERED", TA_ROUTE_DELIVERED);
    constant(environment, o, "ROUTE_RELAYED", TA_ROUTE_RELAYED);
    constant(environment, o, "ROUTE_DUPLICATE", TA_ROUTE_DUPLICATE);
    constant(environment, o, "ROUTE_REFUSED", TA_ROUTE_REFUSED);
    constant(environment, o, "ROUTE_ERROR_UNKNOWN_TARGET", TA_ROUTE_ERROR_UNKNOWN_TARGET);
    constant(environment, o, "ROUTE_ERROR_UNREACHABLE", TA_ROUTE_ERROR_UNREACHABLE);
    constant(environment, o, "ROUTE_ERROR_DENIED", TA_ROUTE_ERROR_DENIED);
    constant(environment, o, "ROUTE_ERROR_EXPIRED", TA_ROUTE_ERROR_EXPIRED);
    constant(environment, o, "ROUTE_ERROR_HOPS", TA_ROUTE_ERROR_HOPS);
    constant(environment, o, "ROUTE_ERROR_MALFORMED", TA_ROUTE_ERROR_MALFORMED);
    constant(environment, o, "EVENT_IN_FROM_INVOKE", TA_EVENT_IN_FROM_INVOKE);
    constant(environment, o, "EVENT_IN_INVOKE_DONE", TA_EVENT_IN_INVOKE_DONE);
    constant(environment, o, "EVENT_IN_ERROR_COMMUNICATION", TA_EVENT_IN_ERROR_COMMUNICATION);
    constant(environment, o, "ACTOR_DONE", TA_ACTOR_DONE);
    constant(environment, o, "ACTOR_HIBERNATED", TA_ACTOR_HIBERNATED);
    constant(environment, o, "STATE_ATOMIC", TA_STATE_ATOMIC);
    constant(environment, o, "STATE_COMPOUND", TA_STATE_COMPOUND);
    constant(environment, o, "STATE_PARALLEL", TA_STATE_PARALLEL);
    constant(environment, o, "STATE_FINAL", TA_STATE_FINAL);
    constant(environment, o, "STATE_HISTORY_SHALLOW", TA_STATE_HISTORY_SHALLOW);
    constant(environment, o, "STATE_HISTORY_DEEP", TA_STATE_HISTORY_DEEP);
    constant(environment, o, "FRAME_MAX", TA_FRAME_MAX);
    constant(environment, o, "TRACEPARENT_LENGTH", TA_TRACEPARENT_LENGTH);
    return o;
}

#define METHOD(name, function)                                                                     \
    { name, NULL, function, NULL, NULL, NULL, napi_default_method, NULL }

napi_value ta_js_init(napi_env environment, napi_value exports) {
    napi_property_descriptor props[] = {
        METHOD("writeXML", js_write_xml),
        METHOD("compile", js_compile),
        METHOD("compileSystem", js_compile_system),
        METHOD("openImage", js_open_image),
        METHOD("imageBytes", js_image_bytes),
        METHOD("imageInfo", js_image_info),
        METHOD("parseAddress", js_parse_address),
        METHOD("formatAddress", js_format_address),
        METHOD("systemIDValid", js_system_valid),
        METHOD("frameHeader", js_frame_header),
        METHOD("frameReader", js_frame_reader),
        METHOD("frameRead", js_frame_read),
        METHOD("stepSpanID", js_step_span_id),
        METHOD("parseTraceparent", js_parse_traceparent),
        METHOD("formatTraceparent", js_format_traceparent),
        METHOD("decodeTraces", js_decode_traces),
        METHOD("decodeMetrics", js_decode_metrics),
        METHOD("metricsToOTLP", js_metrics_to_otlp),
        METHOD("createSystem", js_create_system),
        METHOD("restoreSystem", js_restore_system),
        METHOD("addImage", js_add_image),
        METHOD("chart", js_chart),
        METHOD("boot", js_boot),
        METHOD("actorByName", js_actor_by_name),
        METHOD("actors", js_actors),
        METHOD("spawn", js_spawn),
        METHOD("send", js_send),
        METHOD("sendMany", js_send_many),
        METHOD("post", js_post),
        METHOD("deliver", js_deliver),
        METHOD("pump", js_pump),
        METHOD("now", js_now),
        METHOD("runs", js_runs),
        METHOD("later", js_later),
        METHOD("terminate", js_terminate),
        METHOD("inspect", js_inspect),
        METHOD("stateValue", js_state_value),
        METHOD("context", js_context),
        METHOD("sweep", js_sweep),
        METHOD("hibernate", js_hibernate),
        METHOD("wake", js_wake),
        METHOD("stored", js_stored),
        METHOD("hibernated", js_hibernated),
        METHOD("exportActor", js_export_actor),
        METHOD("adopt", js_adopt),
        METHOD("migrate", js_migrate),
        METHOD("snapshot", js_snapshot),
        METHOD("snapshotContents", js_snapshot_contents),
        METHOD("stats", js_stats),
        METHOD("linkConnect", js_link_connect),
        METHOD("linkServe", js_link_serve),
        METHOD("linkAccept", js_link_accept),
        METHOD("linkOpened", js_link_opened),
        METHOD("linkReceived", js_link_received),
        METHOD("linkDropped", js_link_dropped),
        METHOD("linkAdmit", js_link_admit),
        METHOD("linkClose", js_link_close),
        METHOD("linkStatus", js_link_status),
        METHOD("linkConnections", js_link_connections),
        METHOD("traceConfigure", js_trace_configure),
        METHOD("exporter", js_exporter),
        METHOD("exportAdd", js_export_add),
        METHOD("exportFlush", js_export_flush),
        METHOD("posted", js_posted),
        METHOD("exportStats", js_export_stats),
        METHOD("exportDrain", js_export_drain),
        METHOD("watch", js_watch),
        METHOD("unwatch", js_unwatch),
        METHOD("traceEnd", js_trace_end),
        METHOD("metrics", js_metrics),
        METHOD("destroy", js_destroy),
    };
    napi_define_properties(environment, exports, sizeof props / sizeof props[0], props);
    set(environment, exports, "constants", constants_value(environment));
    set(environment, exports, "traceKinds", trace_kind_names(environment));
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, ta_js_init)
