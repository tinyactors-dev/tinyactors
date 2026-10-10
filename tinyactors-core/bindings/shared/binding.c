/* binding.c — the native side of the JavaScript bindings (Node-API).
 *
 * Both builds compile this file: the Node.js addon against Node's own
 * Node-API, the web module against the synchronous subset in
 * web/src/node_api.h (implemented by web/native.mjs). core.mjs is the JS
 * API on top; this file owns everything that is not JavaScript-specific.
 *
 * Pure functions (hand-written wrappers of the core's public headers):
 *
 *   writeXML(ops, strings) -> xml                      ta_xml_writer_*
 *   compile(sources, files) -> image                   ta_compile_charts
 *   compileSystem(manifest, files) -> image            ta_compile_system
 *   openImage(bytes) -> image, imageBytes(image)       ta_image_open
 *   imageInfo(image) -> { hash, charts, actors }       ta_image_get_info, ta_image_chart_*,
 *                                                      ta_image_state_info, ta_image_boot_actor
 *   parseAddress(text), formatAddress(system, local)   ta_address_*
 *   frameHeader(length), frameReader(max), frameRead   ta_frame_*
 *   parseTraceparent, formatTraceparent, stepSpanID    ta_traceparent_*, ta_step_key_span_id
 *   decodeTraces, decodeMetrics, metricsToOTLP         ta_otlp_next_*, ta_otlp_add_metric
 *   constants                                          numeric constants of the headers
 *
 * A System: one ta_host, its datamodel ("ecmascript": the native one below,
 * or the C one, quickjs-ng), its run loop and effect delivery.
 *
 *   createSystem(callbacks, config) / restoreSystem(callbacks, config, images, host, datamodel)
 *     callbacks: { resolve(source, texts, kinds) -> functions (native ecmascript),
 *                  effects(effects, changed, traces, logs), ref(address) -> value,
 *                  clock() -> ms since the epoch (trace times) }
 *   addImage(system, image) -> segment, chart(system, name) -> [segment, chart] | null
 *   boot(system, segment), actorByName(system, name), actors(system)
 *   spawn(system, segment, chart, name, input, traceparent, now) -> session
 *   send(system, session, name, data, traceparent, now) -> run code
 *   sendMany(system, sessions, names, datas, traceparents, now) -> [accepted, run code]
 *   post(system, session, { name, data, sendid, origin, origintype, raw, traceparent,
 *        flags, token }, now) -> run code              ta_host_send with every field
 *   deliver(system, peer, envelope, now) -> outcome    ta_host_deliver
 *   pump(system, now) -> run code                      the run loop on its own
 *   terminate(system, session, now)
 *   inspect(system, session) -> { segment, chart, state, states } | null
 *   context(system, session) -> the session's variables
 *   sweep, hibernate, wake, exportActor, adopt, migrate, snapshot, stats, peer*,
 *   route, exportName, traceConfigure, traceEnd, metrics, destroy
 *
 * The run loop (pump): ta_host_advance_time, then ta_host_run in rounds
 * until idle or the round limit; every effect goes into one array, handed
 * to callbacks.effects once per call with the sessions whose datamodel saw
 * an event (changed) and the OTLP bytes of the runs. The run code: -2 more
 * work is runnable now, -1 idle without timers, else the next deadline (ms).
 * TA_EFFECT_REQUEST (an <invoke src> that is not a chart of its image) is
 * answered here (ta_host_resume): the chart of that source name in the
 * segment's image, if any. Calls are never nested: JavaScript does not call
 * into a System while it runs (core.mjs queues such calls).
 *
 * The native "ecmascript" datamodel: values are JS values held by
 * references. A session's variables are one object, its scope: <data> ids
 * as properties, and _event, _sessionid, _name, _ioprocessors and In as
 * non-enumerable ones. An expression is a function of the scope, from
 * callbacks.resolve when a chart is attached: a chart's c.expr function, or
 * its text compiled once. Neutral values (docs/protocol.md, "Values") are
 * written and read here (to_wire / from_wire), and blobs, snapshots and
 * envelopes hold them. */
#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors.h>
#include <tinyactors/compile.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/runtime.h>

/* ---- small helpers ------------------------------------------------------------------- */

static char *get_string(napi_env environment, napi_value v, size_t *length_out) {
    size_t length = 0;
    if (napi_get_value_string_utf8(environment, v, NULL, 0, &length) != napi_ok) return NULL;
    char *s = (char *)malloc(length + 1);
    if (!s) return NULL;
    napi_get_value_string_utf8(environment, v, s, length + 1, &length);
    if (length_out) *length_out = length;
    return s;
}

static napi_value str_value(napi_env environment, ta_string_view s) {
    napi_value v;
    napi_create_string_utf8(environment, s.pointer ? s.pointer : "", s.length, &v);
    return v;
}

static napi_value undef(napi_env environment) {
    napi_value u;
    napi_get_undefined(environment, &u);
    return u;
}

static napi_value null_value(napi_env environment) {
    napi_value r;
    napi_get_null(environment, &r);
    return r;
}

static napi_value bool_value(napi_env environment, int b) {
    napi_value r;
    napi_get_boolean(environment, b, &r);
    return r;
}

static napi_value num(napi_env environment, double d) {
    napi_value v;
    napi_create_double(environment, d, &v);
    return v;
}

static napi_valuetype type_of(napi_env environment, napi_value v) {
    napi_valuetype t = napi_undefined;
    if (v) napi_typeof(environment, v, &t);
    return t;
}

static uint32_t get_u32(napi_env environment, napi_value v) {
    uint32_t x = 0;
    napi_get_value_uint32(environment, v, &x);
    return x;
}

static double get_double(napi_env environment, napi_value v) {
    double d = 0;
    napi_get_value_double(environment, v, &d);
    return d;
}

/* Session ids cross as numbers: this host numbers sessions segment << 32 | n, below 2^53. */
static uint64_t get_session(napi_env environment, napi_value v) {
    double d = get_double(environment, v);
    return d > 0 ? (uint64_t)d : 0;
}

/* References are boxed ({ v: value }): before Node-API 10 a reference can
 * only hold objects, functions and symbols, but values may be primitives. */
static napi_ref make_ref(napi_env environment, napi_value v) {
    napi_value box;
    napi_ref r = NULL;
    if (napi_create_object(environment, &box) != napi_ok) return NULL;
    napi_set_named_property(environment, box, "v", v);
    napi_create_reference(environment, box, 1, &r);
    return r;
}

static napi_value get_ref(napi_env environment, napi_ref r) {
    napi_value box = NULL, v = NULL;
    if (r && napi_get_reference_value(environment, r, &box) == napi_ok && box)
        napi_get_named_property(environment, box, "v", &v);
    return v ? v : undef(environment);
}

static void set_ref(napi_env environment, napi_ref *r, napi_value v) {
    if (*r) napi_delete_reference(environment, *r);
    *r = v ? make_ref(environment, v) : NULL;
}

static napi_value bytes_value(napi_env environment, const void *p, size_t n) {
    napi_value ab, u8;
    void *data = NULL;
    napi_create_arraybuffer(environment, n, &data, &ab);
    if (n) memcpy(data, p, n);
    napi_create_typedarray(environment, napi_uint8_array, n, ab, 0, &u8);
    return u8;
}

static int bytes_of(napi_env environment, napi_value v, void **data, size_t *n) {
    napi_typedarray_type t;
    napi_value ab;
    size_t offset = 0;
    if (type_of(environment, v) != napi_object ||
        napi_get_typedarray_info(environment, v, &t, n, data, &ab, &offset) != napi_ok ||
        t != napi_uint8_array)
        return 0;
    return 1;
}

static napi_value property(napi_env environment, napi_value o, const char *name) {
    napi_value v = NULL;
    if (type_of(environment, o) != napi_object ||
        napi_get_named_property(environment, o, name, &v) != napi_ok)
        return undef(environment);
    return v;
}

/* A string property's text (NULL when it is not a string); the caller frees it. */
static char *property_string(napi_env environment, napi_value o, const char *name, size_t *n) {
    napi_value v = property(environment, o, name);
    *n = 0;
    return type_of(environment, v) == napi_string ? get_string(environment, v, n) : NULL;
}

static double property_double(napi_env environment, napi_value o, const char *name) {
    napi_value v = property(environment, o, name);
    return type_of(environment, v) == napi_number ? get_double(environment, v) : 0;
}

static uint32_t array_length(napi_env environment, napi_value v) {
    bool is_array = false;
    uint32_t n = 0;
    if (v && napi_is_array(environment, v, &is_array) == napi_ok && is_array)
        napi_get_array_length(environment, v, &n);
    return n;
}

static void clear_exception(napi_env environment) {
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    if (pending) {
        napi_value ignored;
        napi_get_and_clear_last_exception(environment, &ignored);
    }
}

/* Call a global's method, e.g. JSON.parse. */
static napi_value call_global(napi_env environment, const char *object, const char *method,
                              size_t argc, napi_value *argv) {
    napi_value global, o, f, r = NULL;
    napi_get_global(environment, &global);
    napi_get_named_property(environment, global, object, &o);
    napi_get_named_property(environment, o, method, &f);
    if (napi_call_function(environment, o, f, argc, argv, &r) != napi_ok) return NULL;
    return r;
}

static napi_value call(napi_env environment, napi_value f, size_t argc, napi_value *argv) {
    napi_value global, r = NULL;
    napi_get_global(environment, &global);
    if (napi_call_function(environment, global, f, argc, argv, &r) != napi_ok) return NULL;
    return r;
}

static void set(napi_env environment, napi_value o, const char *name, napi_value v) {
    napi_set_named_property(environment, o, name, v);
}

/* A growable byte buffer (JSON text, blobs). */
typedef struct text {
    char *bytes;
    size_t length, capacity;
    int failed;
} text;

static int text_room(text *t, size_t more) {
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

static void text_put(text *t, const char *p, size_t n) {
    if (n && text_room(t, n)) {
        memcpy(t->bytes + t->length, p, n);
        t->length += n;
    }
}

static void text_char(text *t, char c) {
    text_put(t, &c, 1);
}

static void text_string(text *t, const char *p, size_t n) { /* a JSON string */
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

static ta_string_view view_of(const char *p, size_t n) {
    return (ta_string_view){p ? p : "", (uint32_t)n};
}

/* ---- base64 (RFC 4648, standard alphabet, padded): $bytes on the wire --------------- */

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_put(text *t, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) |
                     (i + 2 < n ? p[i + 2] : 0);
        char out[4] = {base64_alphabet[v >> 18 & 63], base64_alphabet[v >> 12 & 63],
                       i + 1 < n ? base64_alphabet[v >> 6 & 63] : '=',
                       i + 2 < n ? base64_alphabet[v & 63] : '='};
        text_put(t, out, 4);
    }
}

static int base64_digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes into out (n / 4 * 3 bytes at most); returns the length, or -1. */
static long base64_read(const char *s, size_t n, unsigned char *out) {
    if (n % 4) return -1;
    long k = 0;
    for (size_t i = 0; i < n; i += 4) {
        int d[4];
        for (int j = 0; j < 4; j++) {
            d[j] = s[i + j] == '=' && i + 4 == n && j >= 2 ? 0 : base64_digit(s[i + j]);
            if (d[j] < 0) return -1;
        }
        uint32_t v =
            (uint32_t)d[0] << 18 | (uint32_t)d[1] << 12 | (uint32_t)d[2] << 6 | (uint32_t)d[3];
        out[k++] = (unsigned char)(v >> 16);
        if (s[i + 2] != '=') out[k++] = (unsigned char)(v >> 8);
        if (s[i + 3] != '=') out[k++] = (unsigned char)v;
    }
    return k;
}

/* ---- neutral values (docs/protocol.md, "Values") ----------------------------------------
 * Written from JS values: JSON, Uint8Array as {"$bytes"}, an object with a
 * toJSON method as what it returns (Date: its text; a reference, whose
 * toJSON gives {"$ref": address}: that), an object with a key starting with
 * "$" escaped as {"$": ...}. Undefined and functions are left out of objects
 * and are null elsewhere, BigInt is its decimal text, non-finite numbers are
 * null. A cycle (deeper than 128) is TA_ERROR_INVALID. Read back with
 * JSON.parse and the reverse rewrites ($ref through callbacks.ref). */

static ta_status wire_value(napi_env environment, napi_value v, text *t, int depth);

static void wire_number(text *t, double d) {
    char buffer[40];
    if (d != d || d > 1.7976931348623157e308 || d < -1.7976931348623157e308) {
        text_put(t, "null", 4);
        return;
    }
    if (d == 0) {
        text_char(t, '0');
        return;
    }
    for (int precision = 15; precision <= 17; precision++) { /* the shortest that reads back */
        snprintf(buffer, sizeof buffer, "%.*g", precision, d);
        if (strtod(buffer, NULL) == d) break;
    }
    text_put(t, buffer, strlen(buffer));
}

/* 1: a property worth writing (not undefined, a function or a symbol) */
static int wire_present(napi_env environment, napi_value v) {
    napi_valuetype k = type_of(environment, v);
    return k != napi_undefined && k != napi_function && k != napi_symbol;
}

static ta_status wire_object(napi_env environment, napi_value v, text *t, int depth) {
    napi_value keys;
    uint32_t n = 0;
    if (napi_get_property_names(environment, v, &keys) != napi_ok) return TA_ERROR_INVALID;
    napi_get_array_length(environment, keys, &n);
    int escape = 0;
    for (uint32_t i = 0; i < n && !escape; i++) {
        napi_value key;
        char first[2] = {0, 0};
        size_t length = 0;
        napi_get_element(environment, keys, i, &key);
        if (napi_get_value_string_utf8(environment, key, first, sizeof first, &length) == napi_ok &&
            first[0] == '$')
            escape = 1;
    }
    if (escape) text_put(t, "{\"$\":", 5);
    text_char(t, '{');
    int written = 0;
    for (uint32_t i = 0; i < n; i++) {
        napi_value key, value;
        napi_get_element(environment, keys, i, &key);
        if (napi_get_property(environment, v, key, &value) != napi_ok) return TA_ERROR_INVALID;
        if (!wire_present(environment, value)) continue;
        size_t length = 0;
        char *k = get_string(environment, key, &length);
        if (!k) return TA_ERROR_INVALID;
        if (written++) text_char(t, ',');
        text_string(t, k, length);
        free(k);
        text_char(t, ':');
        ta_status status = wire_value(environment, value, t, depth + 1);
        if (status != TA_OK) return status;
    }
    text_char(t, '}');
    if (escape) text_char(t, '}');
    return TA_OK;
}

static ta_status wire_value(napi_env environment, napi_value v, text *t, int depth) {
    if (depth > 128) return TA_ERROR_INVALID;
    switch (type_of(environment, v)) {
    case napi_null:
    case napi_undefined:
    case napi_function:
    case napi_symbol:
        text_put(t, "null", 4);
        return TA_OK;
    case napi_boolean: {
        bool b = false;
        napi_get_value_bool(environment, v, &b);
        text_put(t, b ? "true" : "false", b ? 4 : 5);
        return TA_OK;
    }
    case napi_number:
        wire_number(t, get_double(environment, v));
        return TA_OK;
    case napi_string:
    case napi_bigint: {
        napi_value s = v;
        size_t n = 0;
        if (type_of(environment, v) == napi_bigint &&
            napi_coerce_to_string(environment, v, &s) != napi_ok)
            return TA_ERROR_INVALID;
        char *p = get_string(environment, s, &n);
        if (!p) return TA_ERROR_INVALID;
        text_string(t, p, n);
        free(p);
        return TA_OK;
    }
    case napi_object:
        break;
    default:
        return TA_ERROR_INVALID;
    }
    void *data = NULL;
    size_t n = 0;
    if (bytes_of(environment, v, &data, &n)) {
        text_put(t, "{\"$bytes\":\"", 11);
        base64_put(t, (const unsigned char *)data, n);
        text_put(t, "\"}", 2);
        return TA_OK;
    }
    napi_value to_json = property(environment, v, "toJSON");
    if (type_of(environment, to_json) == napi_function) {
        napi_value r = NULL;
        if (napi_call_function(environment, v, to_json, 0, NULL, &r) != napi_ok) {
            clear_exception(environment);
            return TA_ERROR_INVALID;
        }
        napi_value address = property(environment, r, "$ref");
        if (type_of(environment, address) == napi_string) {
            size_t length = 0;
            char *p = get_string(environment, address, &length);
            text_put(t, "{\"$ref\":", 8);
            text_string(t, p, length);
            text_char(t, '}');
            free(p);
            return TA_OK;
        }
        return wire_value(environment, r, t, depth + 1);
    }
    bool is_array = false;
    napi_is_array(environment, v, &is_array);
    if (!is_array) return wire_object(environment, v, t, depth);
    uint32_t count = 0;
    napi_get_array_length(environment, v, &count);
    text_char(t, '[');
    for (uint32_t i = 0; i < count; i++) {
        napi_value element;
        if (i) text_char(t, ',');
        napi_get_element(environment, v, i, &element);
        ta_status status = wire_value(environment, element, t, depth + 1);
        if (status != TA_OK) return status;
    }
    text_char(t, ']');
    return TA_OK;
}

/* A JS value as neutral JSON text in *t (t->failed: no memory). */
static ta_status wire_of(napi_env environment, napi_value v, text *t) {
    t->length = 0;
    ta_status status = wire_value(environment, v, t, 0);
    if (status == TA_OK && t->failed) status = TA_ERROR_NO_MEMORY;
    return status;
}

/* JSON.parse's result with the neutral forms read: {"$bytes"}, {"$ref"}
 * (refs: callbacks.ref, or the address itself), {"$": ...} (its object's keys
 * literally, its values read). */
static napi_value unwire_children(napi_env environment, napi_value v, napi_value refs, int depth);

static napi_value unwire(napi_env environment, napi_value v, napi_value refs, int depth) {
    if (depth > 512 || type_of(environment, v) != napi_object) return v;
    bool is_array = false;
    napi_is_array(environment, v, &is_array);
    if (is_array) return unwire_children(environment, v, refs, depth);
    napi_value keys, key, inner;
    uint32_t n = 0;
    napi_get_property_names(environment, v, &keys);
    napi_get_array_length(environment, keys, &n);
    if (n == 1) {
        char name[8] = {0};
        size_t length = 0;
        napi_get_element(environment, keys, 0, &key);
        napi_get_value_string_utf8(environment, key, name, sizeof name, &length);
        napi_get_property(environment, v, key, &inner);
        if (length == 6 && !strcmp(name, "$bytes") && type_of(environment, inner) == napi_string) {
            size_t sl = 0;
            char *s = get_string(environment, inner, &sl);
            unsigned char *out = (unsigned char *)malloc(sl / 4 * 3 + 1);
            long k = s && out ? base64_read(s, sl, out) : -1;
            napi_value r = k >= 0 ? bytes_value(environment, out, (size_t)k) : v;
            free(s);
            free(out);
            return r;
        }
        if (length == 4 && !strcmp(name, "$ref") && type_of(environment, inner) == napi_string) {
            napi_value r = type_of(environment, refs) == napi_function
                               ? call(environment, refs, 1, &inner)
                               : NULL;
            if (!r) clear_exception(environment);
            return r ? r : inner;
        }
        if (length == 1 && name[0] == '$' && type_of(environment, inner) == napi_object)
            return unwire_children(environment, inner, refs, depth);
    }
    return unwire_children(environment, v, refs, depth);
}

static napi_value unwire_children(napi_env environment, napi_value v, napi_value refs, int depth) {
    bool is_array = false;
    napi_is_array(environment, v, &is_array);
    if (is_array) {
        uint32_t n = 0;
        napi_get_array_length(environment, v, &n);
        for (uint32_t i = 0; i < n; i++) {
            napi_value element;
            napi_get_element(environment, v, i, &element);
            if (type_of(environment, element) == napi_object)
                napi_set_element(environment, v, i, unwire(environment, element, refs, depth + 1));
        }
        return v;
    }
    napi_value keys;
    uint32_t n = 0;
    napi_get_property_names(environment, v, &keys);
    napi_get_array_length(environment, keys, &n);
    for (uint32_t i = 0; i < n; i++) {
        napi_value key, element;
        napi_get_element(environment, keys, i, &key);
        napi_get_property(environment, v, key, &element);
        if (type_of(environment, element) == napi_object)
            napi_set_property(environment, v, key, unwire(environment, element, refs, depth + 1));
    }
    return v;
}

/* Neutral JSON text -> a JS value; NULL if it does not parse (no exception left). */
static napi_value value_of_wire(napi_env environment, ta_string_view json, napi_value refs) {
    napi_value s = str_value(environment, json),
               parsed = call_global(environment, "JSON", "parse", 1, &s);
    if (!parsed) {
        clear_exception(environment);
        return NULL;
    }
    return unwire(environment, parsed, refs, 0);
}

/* ---- images ------------------------------------------------------------------------- */

typedef struct nd_image {
    void *bytes;
    size_t length;
    ta_image image;
} nd_image;

static void image_finalize(napi_env environment, void *data, void *hint) {
    (void)environment;
    (void)hint;
    nd_image *image = (nd_image *)data;
    free(image->bytes);
    free(image);
}

static napi_value image_value(napi_env environment, const void *bytes, size_t length) {
    nd_image *image = (nd_image *)calloc(1, sizeof *image);
    napi_value r = NULL;
    if (!image || !(image->bytes = malloc(length + 8))) {
        free(image);
        napi_throw_error(environment, NULL, "out of memory");
        return NULL;
    }
    memcpy(image->bytes, bytes, length);
    image->length = length;
    if (ta_image_open(image->bytes, image->length, &image->image) != TA_OK) {
        image_finalize(environment, image, NULL);
        napi_throw_error(environment, NULL, "not a valid tinyactors image");
        return NULL;
    }
    napi_create_external(environment, image, image_finalize, NULL, &r);
    return r;
}

static nd_image *get_image(napi_env environment, napi_value v) {
    void *p = NULL;
    if (type_of(environment, v) != napi_external ||
        napi_get_value_external(environment, v, &p) != napi_ok || !p) {
        napi_throw_type_error(environment, NULL, "expected an image");
        return NULL;
    }
    return (nd_image *)p;
}

/* src="..." at compile time (<invoke src>, <data src>, <script src>): the files
 * passed along, by name (file: and ./ stripped), else by base name */
typedef struct nd_files {
    const ta_source *source;
    uint32_t count;
} nd_files;

static ta_string_view source_name(ta_string_view source) {
    static const char *const prefix[] = {"file://", "file:", "./"};
    for (int k = 0; k < 3; k++) {
        uint32_t l = (uint32_t)strlen(prefix[k]);
        if (source.length > l && !memcmp(source.pointer, prefix[k], l)) {
            source.pointer += l;
            source.length -= l;
        }
    }
    return source;
}

static ta_status nd_resolve(void *context, ta_string_view base, ta_string_view source,
                            ta_source *out) {
    (void)base;
    const nd_files *files = (const nd_files *)context;
    source = source_name(source);
    ta_string_view base_name = source;
    for (uint32_t i = 0; i < source.length; i++)
        if (source.pointer[i] == '/')
            base_name = (ta_string_view){source.pointer + i + 1, source.length - i - 1};
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < files->count; i++) {
            ta_string_view want = pass ? base_name : source;
            if (files->source[i].name.length == want.length &&
                !memcmp(files->source[i].name.pointer, want.pointer, want.length)) {
                *out = files->source[i];
                return TA_OK;
            }
        }
    return TA_ERROR_INVALID;
}

/* {name, xml}[] -> sources (strings owned in `owned`); 0 on a type error (thrown) */
static int read_sources(napi_env environment, napi_value array, uint32_t n, ta_source *source,
                        char **owned) {
    for (uint32_t i = 0; i < n; i++) {
        napi_value o;
        size_t nl = 0, xl = 0;
        napi_get_element(environment, array, i, &o);
        owned[2 * i] = property_string(environment, o, "name", &nl);
        owned[2 * i + 1] = property_string(environment, o, "xml", &xl);
        if (!owned[2 * i] || !owned[2 * i + 1]) {
            napi_throw_type_error(environment, NULL, "compile: name and xml must be strings");
            return 0;
        }
        source[i].name = view_of(owned[2 * i], nl);
        source[i].text = view_of(owned[2 * i + 1], xl);
    }
    return 1;
}

static void throw_diagnostics(napi_env environment, const char *what, ta_compile_result *compiled) {
    char message[4096];
    snprintf(message, sizeof message, "%s", what);
    const ta_diagnostic *diagnostic;
    uint32_t nd = ta_compile_diagnostics(compiled, &diagnostic);
    for (uint32_t i = 0; i < nd && strlen(message) < sizeof message - 200; i++)
        snprintf(message + strlen(message), sizeof message - strlen(message),
                 "\n  %.*s:%u:%u: %.*s", (int)diagnostic[i].file.length,
                 diagnostic[i].file.pointer ? diagnostic[i].file.pointer : "", diagnostic[i].line,
                 diagnostic[i].column, (int)diagnostic[i].message.length,
                 diagnostic[i].message.pointer);
    napi_throw_error(environment, NULL, message);
}

/* compile(sources: {name, xml}[], files?: {name, xml}[]) -> image: charts, plus
 * files their src="..." attributes may name. manifest: compileSystem. */
static napi_value compile_any(napi_env environment, napi_callback_info info, int system) {
    size_t argc = 2;
    napi_value argv[2], result = NULL;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    uint32_t n = system ? 1
                 : argc ? array_length(environment, argv[0])
                        : 0,
             nf = argc > 1 ? array_length(environment, argv[1]) : 0;
    if (!argc || (!system && type_of(environment, argv[0]) != napi_object) ||
        (system && type_of(environment, argv[0]) != napi_string)) {
        napi_throw_type_error(environment, NULL,
                              system ? "compileSystem(manifest: string, files?: {name, xml}[])"
                                     : "compile(sources: {name, xml}[], files?: {name, xml}[])");
        return NULL;
    }
    ta_source *source = (ta_source *)calloc(n + nf + 1, sizeof *source);
    char **owned = (char **)calloc(2 * (n + nf + 1), sizeof(char *));
    if (system) {
        size_t ml = 0;
        owned[1] = get_string(environment, argv[0], &ml);
        source[0].name = view_of("system.xml", 10);
        source[0].text = view_of(owned[1], ml);
    } else if (!read_sources(environment, argv[0], n, source, owned))
        goto out;
    if (nf && !read_sources(environment, argv[1], nf, source + n, owned + 2 * n)) goto out;
    {
        nd_files files = {source + n, nf};
        ta_compile_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.resolve = nd_resolve;
        options.resolve_context = &files;
        ta_compile_result *compiled = NULL;
        ta_status status = system ? ta_compile_system(&options, source, &compiled)
                                  : ta_compile_charts(&options, source, n, &compiled);
        if (status != TA_OK) {
            throw_diagnostics(
                environment,
                system ? "system compilation failed:" : "SCXML compilation failed:", compiled);
        } else {
            ta_string_view bytes = ta_compile_image(compiled);
            result = image_value(environment, bytes.pointer, bytes.length);
        }
        ta_compile_free(compiled);
    }
out:
    for (uint32_t i = 0; i < 2 * (n + nf + 1); i++)
        free(owned[i]);
    free(owned);
    free(source);
    return result;
}

static napi_value js_compile(napi_env environment, napi_callback_info info) {
    return compile_any(environment, info, 0);
}

static napi_value js_compile_system(napi_env environment, napi_callback_info info) {
    return compile_any(environment, info, 1);
}

/* writeXML(ops: Uint8Array, strings: string[]) -> string: XML from an element
 * tree, through ta_xml_writer. ops: 1 open (takes a name from `strings`), 2
 * attribute (a name and a value), 3 text (one), 4 close. */
static napi_value js_write_xml(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], result = NULL;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *data = NULL;
    size_t count = 0;
    if (argc < 2 || !bytes_of(environment, argv[0], &data, &count) ||
        type_of(environment, argv[1]) != napi_object) {
        napi_throw_type_error(environment, NULL, "writeXML(ops: Uint8Array, strings: string[])");
        return NULL;
    }
    const uint8_t *ops = (const uint8_t *)data;
    uint32_t string_count = array_length(environment, argv[1]), next = 0;
    ta_xml_writer *writer = NULL;
    if (ta_xml_writer_new(NULL, &writer) != TA_OK) {
        napi_throw_error(environment, NULL, "writeXML: out of memory");
        return NULL;
    }
    ta_status status = TA_OK;
    for (size_t i = 0; i < count && status == TA_OK; i++) {
        uint32_t needs = ops[i] == 1 || ops[i] == 3 ? 1 : ops[i] == 2 ? 2 : 0;
        char *texts[2] = {NULL, NULL};
        ta_string_view view[2] = {{NULL, 0}, {NULL, 0}};
        if (ops[i] < 1 || ops[i] > 4 || next + needs > string_count) status = TA_ERROR_INVALID;
        for (uint32_t k = 0; k < needs && status == TA_OK; k++) {
            napi_value element;
            size_t length = 0;
            napi_get_element(environment, argv[1], next++, &element);
            texts[k] = get_string(environment, element, &length);
            if (!texts[k]) status = TA_ERROR_INVALID;
            view[k] = view_of(texts[k], length);
        }
        if (status == TA_OK) {
            if (ops[i] == 1) status = ta_xml_writer_open(writer, view[0]);
            if (ops[i] == 2) status = ta_xml_writer_attribute(writer, view[0], view[1]);
            if (ops[i] == 3) status = ta_xml_writer_text(writer, view[0]);
            if (ops[i] == 4) status = ta_xml_writer_close(writer);
        }
        free(texts[0]);
        free(texts[1]);
    }
    ta_string_view xml = {NULL, 0};
    if (status == TA_OK) status = ta_xml_writer_finish(writer, &xml);
    if (status == TA_OK)
        result = str_value(environment, xml);
    else
        napi_throw_error(environment, NULL,
                         status == TA_ERROR_NO_MEMORY
                             ? "writeXML: out of memory"
                             : "writeXML: not a well-formed element tree (an element or "
                               "attribute name that is not an XML name, a control character, "
                               "or an attribute after content)");
    ta_xml_writer_free(writer);
    return result;
}

/* openImage(bytes) -> image (copied) */
static napi_value js_open_image(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *data = NULL;
    size_t n = 0;
    if (!argc || !bytes_of(environment, argv[0], &data, &n)) {
        napi_throw_type_error(environment, NULL, "openImage(bytes: Uint8Array)");
        return NULL;
    }
    return image_value(environment, data, n);
}

static napi_value js_image_bytes(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_image *image = argc ? get_image(environment, argv[0]) : NULL;
    return image ? bytes_value(environment, image->bytes, image->length) : NULL;
}

static napi_value hash_value(napi_env environment, uint64_t hash) {
    char text[17];
    snprintf(text, sizeof text, "%016llx", (unsigned long long)hash);
    return str_value(environment, view_of(text, 16));
}

/* imageInfo(image) -> { hash, charts: [{ name, source, datamodel, states: [{ id,
 * parent, kind }] }], actors: [{ name, chart }] } */
static napi_value js_image_info(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], o, charts, actors;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_image *image = argc ? get_image(environment, argv[0]) : NULL;
    if (!image) return NULL;
    ta_image_info image_info;
    image_info.size = sizeof image_info;
    ta_image_get_info(&image->image, &image_info);
    napi_create_object(environment, &o);
    set(environment, o, "hash", hash_value(environment, image_info.content_hash));
    napi_create_array_with_length(environment, image_info.chart_count, &charts);
    for (uint32_t c = 0; c < image_info.chart_count; c++) {
        napi_value x, states;
        napi_create_object(environment, &x);
        set(environment, x, "name", str_value(environment, ta_image_chart_name(&image->image, c)));
        set(environment, x, "source",
            str_value(environment, ta_image_chart_source(&image->image, c)));
        set(environment, x, "datamodel",
            str_value(environment, ta_image_chart_datamodel(&image->image, c)));
        uint32_t n = ta_image_state_count(&image->image, c);
        napi_create_array_with_length(environment, n, &states);
        for (uint32_t s = 0; s < n; s++) {
            ta_state_info state;
            state.size = sizeof state;
            if (ta_image_state_info(&image->image, c, s, &state) != TA_OK) break;
            napi_value y;
            napi_create_object(environment, &y);
            set(environment, y, "id", str_value(environment, state.id));
            set(environment, y, "parent",
                num(environment, state.parent == TA_NIL ? -1.0 : (double)state.parent));
            set(environment, y, "kind", num(environment, state.kind));
            napi_set_element(environment, states, s, y);
        }
        set(environment, x, "states", states);
        napi_set_element(environment, charts, c, x);
    }
    set(environment, o, "charts", charts);
    napi_create_array_with_length(environment, image_info.boot_actor_count, &actors);
    for (uint32_t i = 0; i < image_info.boot_actor_count; i++) {
        ta_string_view name = {NULL, 0};
        uint32_t chart = 0;
        napi_value x;
        napi_create_object(environment, &x);
        if (ta_image_boot_actor(&image->image, i, &name, &chart) == TA_OK) {
            set(environment, x, "name", str_value(environment, name));
            set(environment, x, "chart", num(environment, chart));
        }
        napi_set_element(environment, actors, i, x);
    }
    set(environment, o, "actors", actors);
    return o;
}

/* ---- addresses (docs/protocol.md, "Addresses") ---------------------------------------- */

static napi_value decimal_value(napi_env environment, uint64_t value) {
    char text[24];
    int n = snprintf(text, sizeof text, "%llu", (unsigned long long)value);
    return str_value(environment, view_of(text, (size_t)n));
}

static uint64_t decimal_of(napi_env environment, napi_value v) {
    size_t n = 0;
    napi_value s = v;
    if (type_of(environment, v) != napi_string &&
        napi_coerce_to_string(environment, v, &s) != napi_ok)
        return 0;
    char *t = get_string(environment, s, &n);
    uint64_t value = 0;
    for (size_t i = 0; t && i < n && t[i] >= '0' && t[i] <= '9'; i++)
        value = value * 10 + (uint64_t)(t[i] - '0');
    free(t);
    return value;
}

/* parseAddress(text) -> { system, local, kind: 'session' | 'name', session: decimal,
 * name (decoded), encoded } | null */
static napi_value js_parse_address(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], o;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (!argc || type_of(environment, argv[0]) != napi_string) return null_value(environment);
    size_t n = 0;
    char *t = get_string(environment, argv[0], &n);
    ta_address address;
    napi_value result = null_value(environment);
    if (t && ta_address_parse(view_of(t, n), &address) == TA_OK) {
        napi_create_object(environment, &o);
        set(environment, o, "system", str_value(environment, address.system));
        set(environment, o, "local", str_value(environment, address.local));
        if (address.kind == TA_ADDRESS_SESSION) {
            set(environment, o, "kind", str_value(environment, view_of("session", 7)));
            set(environment, o, "session", decimal_value(environment, address.session));
            result = o;
        } else {
            char *decoded = (char *)malloc(address.name.length + 1);
            uint32_t length =
                decoded ? ta_address_name_decode(address.name, decoded, address.name.length + 1)
                        : 0;
            if (length) {
                set(environment, o, "kind", str_value(environment, view_of("name", 4)));
                set(environment, o, "name", str_value(environment, view_of(decoded, length)));
                set(environment, o, "encoded", str_value(environment, address.name));
                result = o;
            }
            free(decoded);
        }
    }
    free(t);
    return result;
}

/* formatAddress(system, local) -> text: local is a session id (number, bigint
 * or decimal string with "#_scxml_") or a name; throws if invalid */
static napi_value js_format_address(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (argc < 2 || type_of(environment, argv[0]) != napi_string) {
        napi_throw_type_error(environment, NULL, "formatAddress(system: string, local)");
        return NULL;
    }
    size_t sl = 0, nl = 0;
    char *system = get_string(environment, argv[0], &sl), buffer[TA_ADDRESS_SESSION_MAX];
    uint32_t length = 0;
    napi_valuetype t = type_of(environment, argv[1]);
    napi_value result = NULL;
    if (t == napi_number || t == napi_bigint) {
        length = ta_address_format(view_of(system, sl), decimal_of(environment, argv[1]), buffer,
                                   sizeof buffer);
        if (length) result = str_value(environment, view_of(buffer, length));
    } else {
        char *name = get_string(environment, argv[1], &nl);
        uint32_t capacity = (uint32_t)(sl + 3 * nl + 32);
        char *out = (char *)malloc(capacity);
        length = name && nl && out
                     ? ta_address_format_name(view_of(system, sl), view_of(name, nl), out, capacity)
                     : 0;
        if (length) result = str_value(environment, view_of(out, length));
        free(out);
        free(name);
    }
    free(system);
    if (!result)
        napi_throw_type_error(environment, NULL,
                              "formatAddress: not a valid system id, session id or name");
    return result;
}

static napi_value js_system_valid(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (!argc || type_of(environment, argv[0]) != napi_string) return bool_value(environment, 0);
    size_t n = 0;
    char *t = get_string(environment, argv[0], &n);
    int valid = t && ta_address_system_valid(view_of(t, n));
    free(t);
    return bool_value(environment, valid);
}

/* ---- frames (docs/protocol.md, "Framing") ----------------------------------------------- */

/* frameHeader(length) -> Uint8Array(4) */
static napi_value js_frame_header(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    uint8_t header[TA_FRAME_HEADER];
    ta_frame_header(argc ? get_u32(environment, argv[0]) : 0, header);
    return bytes_value(environment, header, sizeof header);
}

typedef struct nd_frames {
    ta_frame_reader reader;
    uint8_t *buffer;
} nd_frames;

static void frames_finalize(napi_env environment, void *data, void *hint) {
    (void)environment;
    (void)hint;
    nd_frames *frames = (nd_frames *)data;
    free(frames->buffer);
    free(frames);
}

/* frameReader(maxFrame) -> reader */
static napi_value js_frame_reader(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], r;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    uint32_t max = argc ? get_u32(environment, argv[0]) : 0;
    nd_frames *frames = (nd_frames *)calloc(1, sizeof *frames);
    if (!max) max = TA_FRAME_MAX;
    if (!frames || !(frames->buffer = (uint8_t *)malloc(max))) {
        free(frames);
        napi_throw_error(environment, NULL, "frameReader: out of memory");
        return NULL;
    }
    frames->reader.size = sizeof frames->reader;
    ta_frame_reader_init(&frames->reader, max);
    napi_create_external(environment, frames, frames_finalize, NULL, &r);
    return r;
}

/* frameRead(reader, chunk) -> [Uint8Array (a frame) | number (a refused frame's length)] */
static napi_value js_frame_read(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *p = NULL, *data = NULL;
    size_t n = 0;
    if (argc < 2 || napi_get_value_external(environment, argv[0], &p) != napi_ok || !p ||
        !bytes_of(environment, argv[1], &data, &n)) {
        napi_throw_type_error(environment, NULL, "frameRead(reader, chunk: Uint8Array)");
        return NULL;
    }
    nd_frames *frames = (nd_frames *)p;
    const uint8_t *in = (const uint8_t *)data;
    uint32_t k = 0;
    napi_create_array(environment, &out);
    while (n) {
        uint32_t event = 0,
                 used = ta_frame_read(&frames->reader, frames->buffer, in, (uint32_t)n, &event);
        in += used;
        n -= used;
        if (event == TA_FRAME_READY)
            napi_set_element(environment, out, k++,
                             bytes_value(environment, frames->buffer, frames->reader.length));
        else if (event == TA_FRAME_REFUSED)
            napi_set_element(environment, out, k++, num(environment, frames->reader.length));
    }
    return out;
}

/* ---- trace values ----------------------------------------------------------------------- */

static int hex_text(const char *t, size_t length, uint64_t *high, uint64_t *low) {
    uint64_t parts[2] = {0, 0};
    if (length != 16 && length != 32) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = t[i];
        uint64_t d = c >= '0' && c <= '9'   ? (uint64_t)(c - '0')
                     : c >= 'a' && c <= 'f' ? (uint64_t)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (uint64_t)(c - 'A' + 10)
                                            : 16;
        if (d == 16) return 0;
        size_t half = length == 32 && i < 16 ? 0 : 1;
        parts[half] = parts[half] << 4 | d;
    }
    *high = parts[0];
    *low = parts[1];
    return 1;
}

/* stepSpanID(session, macrostep, microstep) -> 16 hex digits (decimal strings in) */
static napi_value js_step_span_id(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (argc < 3) return NULL;
    ta_step_key key = {decimal_of(environment, argv[0]), decimal_of(environment, argv[1]),
                       get_u32(environment, argv[2]), 0};
    char t[TA_SPAN_ID_TEXT_LENGTH];
    ta_span_id_format(ta_step_key_span_id(key), t, sizeof t);
    return str_value(environment, view_of(t, sizeof t));
}

/* parseTraceparent(text) -> { traceID, parentSpanID, flags } or null */
static napi_value js_parse_traceparent(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (!argc || type_of(environment, argv[0]) != napi_string) return null_value(environment);
    size_t n = 0;
    char *t = get_string(environment, argv[0], &n);
    ta_trace_context context;
    ta_status status = ta_traceparent_parse(view_of(t, n), &context);
    free(t);
    if (status != TA_OK) return null_value(environment);
    char trace[TA_TRACE_ID_TEXT_LENGTH], parent[TA_SPAN_ID_TEXT_LENGTH];
    ta_trace_id_format(context.trace, trace, sizeof trace);
    ta_span_id_format(context.parent, parent, sizeof parent);
    napi_create_object(environment, &out);
    set(environment, out, "traceID", str_value(environment, view_of(trace, sizeof trace)));
    set(environment, out, "parentSpanID", str_value(environment, view_of(parent, sizeof parent)));
    set(environment, out, "flags", num(environment, context.flags));
    return out;
}

/* formatTraceparent(traceID, spanID, flags) -> text, or null if invalid */
static napi_value js_format_traceparent(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    if (argc < 3) return null_value(environment);
    size_t tl = 0, sl = 0;
    char *trace = get_string(environment, argv[0], &tl),
         *span = get_string(environment, argv[1], &sl);
    ta_trace_context context;
    memset(&context, 0, sizeof context);
    uint64_t ignored;
    int ok = trace && span && tl == 32 && sl == 16 &&
             hex_text(trace, tl, &context.trace.high, &context.trace.low) &&
             hex_text(span, sl, &ignored, &context.parent);
    free(trace);
    free(span);
    context.flags = get_u32(environment, argv[2]);
    char t[TA_TRACEPARENT_LENGTH];
    uint32_t n = ok ? ta_traceparent_format(&context, t, sizeof t) : 0;
    return n ? str_value(environment, view_of(t, n)) : null_value(environment);
}
/* ---- decoding: OTLP traces to plain objects (core.mjs shapes them) ------------------------- */

static napi_value decoded_id(napi_env environment, uint64_t high, uint64_t low, int wide) {
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

static napi_value decoded_attributes(napi_env environment, ta_otlp_fields fields);

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
static napi_value decoded_attributes(napi_env environment, ta_otlp_fields fields) {
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
static napi_value js_decode_traces(napi_env environment, napi_callback_info info) {
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

/* ---- the System --------------------------------------------------------------------------- */

typedef struct nd_chart {
    const ta_image *image;
    uint32_t chart, count;
    uint64_t hash;
    napi_ref *function; /* native: per expression id, the function of the scope */
    void *inner;        /* quickjs: its own chart context */
} nd_chart;

typedef struct nd_instance { /* native: one session's datamodel state */
    napi_ref scope, input;
    nd_chart *chart;
    uint64_t serial;
    uint32_t stamp, next_free;
    int live;
    char *string; /* the last eval_string / log result */
    ta_datamodel_environment *outside;
} nd_instance;

typedef struct nd_session { /* quickjs: what the binding knows of one of its instances */
    uint64_t serial;
    const ta_image *image;
    uint32_t chart, stamp;
} nd_session;

typedef struct nd_grave { /* a session that ended in this pump: its last variables */
    uint64_t serial;
    napi_ref context;
} nd_grave;

typedef struct nd_failure { /* an exception in an expression */
    uint64_t serial;
    napi_ref error;
} nd_failure;

#define IO_PROCESSORS_MAX 16u

typedef struct nd_system {
    napi_env environment; /* of the current native call */
    ta_host *host;
    int native; /* the native ecmascript datamodel; else quickjs */
    ta_datamodel_ecmascript *quickjs;
    const ta_datamodel_vtable *quickjs_vtable;
    ta_datamodel_binding binding;
    napi_ref resolve, effects, refs, clock, in_function;
    char system_id[TA_ADDRESS_SYSTEM_MAX + 1];
    ta_io_processor_descriptor io[IO_PROCESSORS_MAX];
    char *io_text[2 * IO_PROCESSORS_MAX];
    uint32_t io_count;
    /* native datamodel: instances (handle = index + 1) and payloads (refcounted) */
    nd_instance *instances;
    uint32_t instance_count, instance_capacity, free_instance, live_instances;
    napi_ref *payloads;
    uint32_t *payload_refs;
    uint32_t payload_count, payload_capacity, free_payload, live_payloads;
    nd_instance *evaluating;
    nd_chart **charts;
    uint32_t chart_count, chart_capacity;
    /* quickjs: instance handle -> session (open addressing) */
    uint64_t *keys;
    nd_session *sessions;
    uint32_t map_capacity, map_count;
    /* the run loop */
    int running, keep_quiet; /* keep_quiet: destroy() is not a session's end (hibernate, export) */
    uint32_t pump;
    napi_value effect_list;
    uint32_t effect_count;
    uint64_t *changed;
    uint32_t changed_count, changed_capacity;
    nd_grave *graves;
    uint32_t grave_count, grave_capacity;
    nd_failure *failures;
    uint32_t failure_count, failure_capacity;
    napi_ref *images; /* per segment: the image, until TA_HOST_EFFECT_SEGMENT_FREED */
    uint32_t image_count, image_capacity;
    text wire;       /* scratch for neutral values */
    char error[512]; /* why the last serialize failed */
    /* tracing (docs/adr/0006 in the repository root) */
    ta_otlp *otlp;
    size_t otlp_length;
    unsigned char *traces, *logs;
    size_t traces_length, traces_capacity, logs_length, logs_capacity;
} nd_system;

static void grow(void **p, uint32_t *capacity, uint32_t need, size_t size) {
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

static void mark_changed(nd_system *system, uint32_t *stamp, uint64_t serial) {
    if (*stamp == system->pump || !serial) return;
    *stamp = system->pump;
    grow((void **)&system->changed, &system->changed_capacity, system->changed_count + 1,
         sizeof(uint64_t));
    system->changed[system->changed_count++] = serial;
}

static uint64_t serial_of(ta_string_view sessionid) {
    uint64_t serial = 0;
    for (uint32_t k = 0; k < sessionid.length; k++)
        if (sessionid.pointer[k] >= '0' && sessionid.pointer[k] <= '9')
            serial = serial * 10 + (uint64_t)(sessionid.pointer[k] - '0');
    return serial;
}

static void grave_add(nd_system *system, uint64_t serial, napi_value context) {
    if (system->keep_quiet || !context) return;
    grow((void **)&system->graves, &system->grave_capacity, system->grave_count + 1,
         sizeof(nd_grave));
    system->graves[system->grave_count].serial = serial;
    system->graves[system->grave_count++].context = make_ref(system->environment, context);
}

/* Record a pending JS exception against a session, and clear it. */
static void take_exception(nd_system *system, uint64_t serial) {
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

static void chart_add(nd_system *system, nd_chart *chart) {
    grow((void **)&system->charts, &system->chart_capacity, system->chart_count + 1,
         sizeof(nd_chart *));
    system->charts[system->chart_count++] = chart;
}

static void chart_remove(nd_system *system, nd_chart *chart) {
    for (uint32_t i = 0; i < system->chart_count; i++)
        if (system->charts[i] == chart) system->charts[i] = system->charts[--system->chart_count];
}

static nd_chart *chart_new(const ta_datamodel_chart *chart) {
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

/* ---- payloads of the native datamodel (refcounted references, free-listed) */

static ta_payload payload_new(nd_system *system, napi_value v) {
    uint32_t i;
    if (system->free_payload) {
        i = system->free_payload - 1;
        system->free_payload = (uint32_t)(uintptr_t)system->payloads[i];
    } else {
        uint32_t c = system->payload_capacity;
        grow((void **)&system->payloads, &c, system->payload_count + 1, sizeof(napi_ref));
        grow((void **)&system->payload_refs, &system->payload_capacity, system->payload_count + 1,
             sizeof(uint32_t));
        i = system->payload_count++;
    }
    system->payloads[i] = make_ref(system->environment, v);
    system->payload_refs[i] = 1;
    system->live_payloads++;
    return (ta_payload)i + 1;
}

static napi_value payload_get(nd_system *system, ta_payload payload) {
    if (!payload || payload > system->payload_count || !system->payload_refs[payload - 1])
        return undef(system->environment);
    return get_ref(system->environment, system->payloads[payload - 1]);
}

static void native_retain(void *d, ta_payload payload) {
    nd_system *system = (nd_system *)d;
    if (payload && payload <= system->payload_count && system->payload_refs[payload - 1])
        system->payload_refs[payload - 1]++;
}

static void native_release(void *d, ta_payload payload) {
    nd_system *system = (nd_system *)d;
    if (!payload || payload > system->payload_count || !system->payload_refs[payload - 1]) return;
    if (--system->payload_refs[payload - 1] == 0) {
        napi_delete_reference(system->environment, system->payloads[payload - 1]);
        system->payloads[payload - 1] = (napi_ref)(uintptr_t)system->free_payload;
        system->free_payload = (uint32_t)payload;
        system->live_payloads--;
    }
}

/* ---- the native "ecmascript" datamodel -------------------------------------------------- */

static nd_instance *instance_at(nd_system *system, uint64_t handle) {
    return handle && handle <= system->instance_count && system->instances[handle - 1].live
               ? &system->instances[handle - 1]
               : NULL;
}

/* In(stateID): the session being evaluated */
static napi_value js_in(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    void *data;
    napi_get_cb_info(environment, info, &argc, argv, NULL, &data);
    nd_system *system = (nd_system *)data;
    nd_instance *instance = system->evaluating;
    size_t length = 0;
    char *id = argc ? get_string(environment, argv[0], &length) : NULL;
    int yes = 0;
    if (id && instance && instance->outside) {
        uint32_t state = ta_image_state_by_name(instance->chart->image, instance->chart->chart,
                                                view_of(id, length));
        yes = state != TA_NIL && ta_datamodel_environment_in(instance->outside, state);
    }
    free(id);
    return bool_value(environment, yes);
}

static void define_hidden(napi_env environment, napi_value o, const char *name, napi_value v,
                          int writable) {
    napi_property_descriptor d;
    memset(&d, 0, sizeof d);
    d.utf8name = name;
    d.value = v;
    d.attributes = 4 | (writable ? 1 : 0); /* napi_configurable (| napi_writable) */
    napi_define_properties(environment, o, 1, &d);
}

/* A new scope: the system variables, read-only and not enumerable. */
static napi_value scope_new(nd_system *system, napi_value sessionid, napi_value name,
                            napi_value io_processors) {
    napi_env environment = system->environment;
    napi_value scope;
    napi_create_object(environment, &scope);
    define_hidden(environment, scope, "_sessionid", sessionid, 0);
    define_hidden(environment, scope, "_name", name, 0);
    define_hidden(environment, scope, "_ioprocessors", io_processors, 0);
    define_hidden(environment, scope, "In", get_ref(environment, system->in_function), 0);
    define_hidden(environment, scope, "_event", undef(environment), 0);
    return scope;
}

static napi_value system_variables_scope(nd_system *system,
                                         const ta_datamodel_system_variables *sv) {
    napi_env environment = system->environment;
    napi_value io;
    napi_create_object(environment, &io);
    for (uint32_t i = 0; i < sv->io_processor_count; i++) {
        napi_value entry;
        napi_create_object(environment, &entry);
        set(environment, entry, "location", str_value(environment, sv->io_processors[i].location));
        napi_set_property(environment, io, str_value(environment, sv->io_processors[i].type),
                          entry);
    }
    return scope_new(system, str_value(environment, sv->sessionid),
                     str_value(environment, sv->name), io);
}

static nd_instance *instance_new(nd_system *system, nd_chart *chart, uint64_t serial,
                                 napi_value scope, uint64_t *out) {
    uint32_t i;
    if (system->free_instance) {
        i = system->free_instance - 1;
        system->free_instance = system->instances[i].next_free;
    } else {
        grow((void **)&system->instances, &system->instance_capacity, system->instance_count + 1,
             sizeof(nd_instance));
        i = system->instance_count++;
    }
    nd_instance *instance = &system->instances[i];
    memset(instance, 0, sizeof *instance);
    instance->chart = chart;
    instance->serial = serial;
    instance->live = 1;
    instance->scope = make_ref(system->environment, scope);
    system->live_instances++;
    *out = i + 1;
    return instance;
}

static void instance_free(nd_system *system, uint64_t handle) {
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return;
    set_ref(system->environment, &instance->scope, NULL);
    set_ref(system->environment, &instance->input, NULL);
    free(instance->string);
    instance->string = NULL;
    instance->live = 0;
    instance->next_free = system->free_instance;
    system->free_instance = (uint32_t)handle;
    system->live_instances--;
}

static void throw_unknown(nd_system *system, nd_instance *instance, ta_expression e) {
    ta_expression_info info;
    char message[600];
    if (ta_image_expression(instance->chart->image, instance->chart->chart, e, &info) == TA_OK)
        snprintf(message, sizeof message, "no implementation for the expression `%.*s`",
                 (int)(info.source.length > 500 ? 500 : info.source.length), info.source.pointer);
    else
        snprintf(message, sizeof message, "unknown expression %u", e);
    napi_throw_error(system->environment, NULL, message);
}

/* Call expression e's function with (scope[, value]); NULL on an exception (recorded). */
static napi_value evaluate(nd_system *system, nd_instance *instance, ta_expression e,
                           napi_value value) {
    nd_chart *chart = instance->chart;
    if (e >= chart->count || !chart->function[e]) {
        throw_unknown(system, instance, e);
        take_exception(system, instance->serial);
        return NULL;
    }
    napi_value argv[2] = {get_ref(system->environment, instance->scope), value}, r = NULL;
    nd_instance *outer = system->evaluating;
    system->evaluating = instance;
    r = call(system->environment, get_ref(system->environment, chart->function[e]), value ? 2 : 1,
             argv);
    system->evaluating = outer;
    if (!r) take_exception(system, instance->serial);
    return r;
}

static ta_status native_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    nd_system *system = (nd_system *)d;
    napi_env environment = system->environment;
    nd_chart *c = chart_new(chart);
    if (!c) return TA_ERROR_NO_MEMORY;
    c->function = (napi_ref *)calloc(c->count ? c->count : 1, sizeof(napi_ref));
    /* resolve(source, texts, kinds) -> functions, aligned with expression ids */
    napi_value texts, kinds, argv[3], result;
    napi_create_array_with_length(environment, c->count, &texts);
    napi_create_array_with_length(environment, c->count, &kinds);
    for (uint32_t e = 0; e < c->count; e++) {
        ta_expression_info info;
        if (ta_image_expression(c->image, c->chart, e, &info) != TA_OK) continue;
        napi_set_element(environment, texts, e, str_value(environment, info.source));
        napi_set_element(environment, kinds, e, num(environment, info.kind));
    }
    argv[0] = str_value(environment, ta_image_chart_source(c->image, c->chart));
    argv[1] = texts;
    argv[2] = kinds;
    result = call(environment, get_ref(environment, system->resolve), 3, argv);
    if (!result) take_exception(system, 0); /* unresolved expressions fail when evaluated */
    for (uint32_t e = 0; result && e < c->count; e++) {
        napi_value f;
        if (napi_get_element(environment, result, e, &f) == napi_ok &&
            type_of(environment, f) == napi_function)
            c->function[e] = make_ref(environment, f);
    }
    chart_add(system, c);
    *context = c;
    return TA_OK;
}

static void native_detach(void *d, void *context) { /* a segment is freed (or the host) */
    nd_system *system = (nd_system *)d;
    nd_chart *chart = (nd_chart *)context;
    if (!chart) return;
    chart_remove(system, chart);
    for (uint32_t e = 0; e < chart->count; e++)
        if (chart->function[e]) napi_delete_reference(system->environment, chart->function[e]);
    free(chart->function);
    free(chart);
}

static ta_status native_create(void *d, void *context, const ta_datamodel_system_variables *sv,
                               ta_payload init, uint64_t *out) {
    nd_system *system = (nd_system *)d;
    uint64_t serial = serial_of(sv->sessionid);
    nd_instance *instance =
        instance_new(system, (nd_chart *)context, serial, system_variables_scope(system, sv), out);
    if (init) instance->input = make_ref(system->environment, payload_get(system, init));
    native_release(system, init); /* the init payload is ours now */
    mark_changed(system, &instance->stamp, serial);
    return TA_OK;
}

static void native_destroy(void *d, uint64_t handle) {
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return;
    grave_add(system, instance->serial, get_ref(system->environment, instance->scope));
    instance_free(system, handle);
}

#define ENTER()                                                                                    \
    nd_system *system = (nd_system *)d;                                                            \
    nd_instance *instance = instance_at(system, handle);                                           \
    if (!instance) return TA_ERROR_EXECUTION;                                                      \
    instance->outside = outside

#define LEAVE(status)                                                                              \
    do {                                                                                           \
        instance->outside = NULL;                                                                  \
        return (status);                                                                           \
    } while (0)

/* <data id expr>: the spawn's input overrides it (a property of that name) */
static ta_status native_init_data(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                  uint32_t declaration) {
    ENTER();
    napi_env environment = system->environment;
    ta_data_info data;
    if (ta_image_data(instance->chart->image, instance->chart->chart, declaration, &data) != TA_OK)
        LEAVE(TA_ERROR_EXECUTION);
    napi_value key = str_value(environment, data.id), v = NULL;
    if (instance->input) {
        napi_value input = get_ref(environment, instance->input);
        if (type_of(environment, input) == napi_object &&
            napi_get_property(environment, input, key, &v) == napi_ok &&
            type_of(environment, v) == napi_undefined)
            v = NULL;
    }
    if (!v)
        v = data.expression == TA_NIL ? undef(environment)
                                      : evaluate(system, instance, data.expression, NULL);
    if (!v) LEAVE(TA_ERROR_EXECUTION);
    napi_set_property(environment, get_ref(environment, instance->scope), key, v);
    LEAVE(TA_OK);
}

static const char *const event_types[] = {"platform", "internal", "external"};

static napi_value event_object(nd_system *system, const ta_event_view *view) {
    napi_env environment = system->environment;
    napi_value e;
    napi_create_object(environment, &e);
    set(environment, e, "name", str_value(environment, view->name));
    set(environment, e, "type",
        str_value(environment, view_of(event_types[view->type < 3 ? view->type : 0],
                                       strlen(event_types[view->type < 3 ? view->type : 0]))));
    if (view->sendid.length) set(environment, e, "sendid", str_value(environment, view->sendid));
    if (view->origin.length) set(environment, e, "origin", str_value(environment, view->origin));
    if (view->origintype.length)
        set(environment, e, "origintype", str_value(environment, view->origintype));
    if (view->invokeid.length)
        set(environment, e, "invokeid", str_value(environment, view->invokeid));
    if (view->raw.length) set(environment, e, "raw", str_value(environment, view->raw));
    if (view->data) set(environment, e, "data", payload_get(system, view->data));
    return e;
}

static ta_status native_bind_event(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                   const ta_event_view *view) {
    (void)outside;
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return TA_ERROR_EXECUTION;
    define_hidden(system->environment, get_ref(system->environment, instance->scope), "_event",
                  event_object(system, view), 0);
    mark_changed(system, &instance->stamp, instance->serial);
    return TA_OK;
}

static ta_status native_condition(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                  ta_expression e, int *out) {
    ENTER();
    napi_value v = evaluate(system, instance, e, NULL), b;
    if (!v) LEAVE(TA_ERROR_EXECUTION);
    bool r = false;
    napi_coerce_to_bool(system->environment, v, &b);
    napi_get_value_bool(system->environment, b, &r);
    *out = r;
    LEAVE(TA_OK);
}

static ta_status to_text(nd_system *system, nd_instance *instance, napi_value v, int json,
                         ta_string_view *out) {
    napi_env environment = system->environment;
    napi_value s = NULL;
    if (json && type_of(environment, v) == napi_object)
        s = call_global(environment, "JSON", "stringify", 1, &v);
    if (!s || type_of(environment, s) != napi_string) {
        clear_exception(environment);
        if (napi_coerce_to_string(environment, v, &s) != napi_ok) {
            take_exception(system, instance->serial);
            return TA_ERROR_EXECUTION;
        }
    }
    size_t length = 0;
    free(instance->string);
    instance->string = get_string(environment, s, &length);
    if (!instance->string) return TA_ERROR_EXECUTION;
    *out = view_of(instance->string, length);
    return TA_OK;
}

static ta_status native_string(void *d, uint64_t handle, ta_datamodel_environment *outside,
                               ta_expression e, ta_string_view *out) {
    ENTER();
    napi_value v = evaluate(system, instance, e, NULL);
    LEAVE(v ? to_text(system, instance, v, 0, out) : TA_ERROR_EXECUTION);
}

static ta_status native_log(void *d, uint64_t handle, ta_datamodel_environment *outside,
                            ta_expression e, ta_string_view *out) {
    ENTER();
    napi_value v = evaluate(system, instance, e, NULL);
    LEAVE(v ? to_text(system, instance, v, 1, out) : TA_ERROR_EXECUTION);
}

/* location expressions resolve to setters: f(scope, value) */
static ta_status native_assign(void *d, uint64_t handle, ta_datamodel_environment *outside,
                               ta_expression location, ta_expression value) {
    ENTER();
    napi_value v = evaluate(system, instance, value, NULL);
    if (!v || !evaluate(system, instance, location, v)) LEAVE(TA_ERROR_EXECUTION);
    LEAVE(TA_OK);
}

static ta_status native_assign_string(void *d, uint64_t handle, ta_expression location,
                                      ta_string_view value) {
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return TA_ERROR_EXECUTION;
    return evaluate(system, instance, location, str_value(system->environment, value))
               ? TA_OK
               : TA_ERROR_EXECUTION;
}

static ta_status native_script(void *d, uint64_t handle, ta_datamodel_environment *outside,
                               ta_expression e) {
    ENTER();
    LEAVE(evaluate(system, instance, e, NULL) ? TA_OK : TA_ERROR_EXECUTION);
}

/* <foreach>: the array (a payload while it runs), its items assigned in turn */
static ta_status native_foreach_begin(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                      ta_expression array, uint64_t *iterator, uint32_t *n) {
    ENTER();
    napi_value v = evaluate(system, instance, array, NULL);
    bool is_array = false;
    if (!v) LEAVE(TA_ERROR_EXECUTION);
    napi_is_array(system->environment, v, &is_array);
    if (!is_array) {
        napi_throw_type_error(system->environment, NULL, "<foreach array>: not an array");
        take_exception(system, instance->serial);
        LEAVE(TA_ERROR_EXECUTION);
    }
    napi_value copy = call_global(system->environment, "Array", "from", 1, &v); /* a shallow copy */
    if (!copy) LEAVE(TA_ERROR_EXECUTION);
    *n = array_length(system->environment, copy);
    *iterator = payload_new(system, copy);
    LEAVE(TA_OK);
}

/* item and index declare their variable when it is not declared (SCXML 4.6) */
static ta_status foreach_assign(nd_system *system, nd_instance *instance, ta_expression location,
                                napi_value value) {
    ta_expression_info info;
    int identifier = ta_image_expression(instance->chart->image, instance->chart->chart, location,
                                         &info) == TA_OK &&
                     info.source.length > 0;
    for (uint32_t k = 0; identifier && k < info.source.length; k++) {
        char c = info.source.pointer[k];
        identifier = c == '_' || c == '$' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (k && c >= '0' && c <= '9');
    }
    if (!identifier)
        return evaluate(system, instance, location, value) ? TA_OK : TA_ERROR_EXECUTION;
    napi_set_property(system->environment, get_ref(system->environment, instance->scope),
                      str_value(system->environment, info.source), value);
    return TA_OK;
}

static ta_status native_foreach_item(void *d, uint64_t handle, uint64_t iterator, uint32_t i,
                                     ta_expression item, ta_expression index) {
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return TA_ERROR_EXECUTION;
    napi_value element;
    napi_get_element(system->environment, payload_get(system, iterator), i, &element);
    if (foreach_assign(system, instance, item, element) != TA_OK) return TA_ERROR_EXECUTION;
    if (index != TA_NIL &&
        foreach_assign(system, instance, index, num(system->environment, i)) != TA_OK)
        return TA_ERROR_EXECUTION;
    return TA_OK;
}

static void native_foreach_end(void *d, uint64_t handle, uint64_t iterator) {
    (void)handle;
    native_release(d, iterator);
}

/* namelist and <param>s: an object of their values; <content>: its value; a
 * trace's value (TA_CAPTURE_VALUE): read with no effect the chart could see */
static ta_status native_capture(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                const ta_datamodel_capture *capture, ta_payload *out) {
    ENTER();
    napi_env environment = system->environment;
    napi_value v = NULL, scope = get_ref(environment, instance->scope);
    const ta_image *image = instance->chart->image;
    uint32_t chart = instance->chart->chart;
    if (capture->kind == TA_CAPTURE_VALUE) {
        ta_expression_info info;
        if (ta_image_expression(image, chart, capture->content, &info) != TA_OK)
            LEAVE(TA_ERROR_EXECUTION);
        napi_get_property(environment, scope, str_value(environment, info.source), &v);
    } else if (capture->content != TA_NIL) {
        v = evaluate(system, instance, capture->content, NULL);
    } else {
        napi_create_object(environment, &v);
        for (uint32_t i = 0; v && i < capture->namelist_count; i++) {
            ta_expression_info info;
            napi_value x;
            if (ta_image_expression(image, chart, capture->namelist[i], &info) != TA_OK) {
                v = NULL;
                break;
            }
            napi_value key = str_value(environment, info.source);
            bool has = false;
            napi_get_property(environment, scope, key, &x);
            napi_value keys;
            napi_get_property_names(environment, scope, &keys); /* a namelist names declared data */
            uint32_t n = array_length(environment, keys);
            for (uint32_t k = 0; k < n && !has; k++) {
                napi_value name;
                napi_get_element(environment, keys, k, &name);
                napi_strict_equals(environment, name, key, &has);
            }
            if (!has) {
                napi_throw_error(environment, NULL, "namelist: not a declared location");
                take_exception(system, instance->serial);
                v = NULL;
                break;
            }
            napi_set_property(environment, v, key, x);
        }
        for (uint32_t i = 0; v && i < capture->param_count; i++) {
            napi_value x = evaluate(system, instance, capture->params[2 * i + 1], NULL);
            if (!x) {
                v = NULL;
                break;
            }
            napi_set_property(
                environment, v,
                str_value(environment, ta_image_string(image, capture->params[2 * i])), x);
        }
    }
    if (!v) LEAVE(TA_ERROR_EXECUTION);
    *out = payload_new(system, v);
    LEAVE(TA_OK);
}

/* hibernation and export: {"v": variables, "e": _event} as neutral values; a
 * variable that cannot be one (a function, a cycle) keeps the session resident */
static ta_status scope_wire(nd_system *system, napi_value scope, text *t) {
    napi_env environment = system->environment;
    napi_value keys;
    napi_get_property_names(environment, scope, &keys);
    uint32_t n = array_length(environment, keys);
    t->length = 0;
    text_put(t, "{\"v\":{", 6);
    int written = 0;
    for (uint32_t i = 0; i < n; i++) {
        napi_value key, value;
        napi_get_element(environment, keys, i, &key);
        napi_get_property(environment, scope, key, &value);
        size_t length = 0;
        char *k = get_string(environment, key, &length);
        napi_valuetype kind = type_of(environment, value);
        ta_status status = TA_OK;
        if (kind == napi_function || kind == napi_symbol) status = TA_ERROR_UNSUPPORTED;
        if (kind == napi_undefined) {
            free(k);
            continue;
        }
        if (status == TA_OK) {
            if (written++) text_char(t, ',');
            text_string(t, k ? k : "", length);
            text_char(t, ':');
            status = wire_value(environment, value, t, 1);
        }
        if (status != TA_OK) {
            snprintf(system->error, sizeof system->error, "variable '%.*s' cannot be stored: %s",
                     (int)(length > 200 ? 200 : length), k ? k : "",
                     kind == napi_function ? "it holds a function"
                                           : "it is not a neutral value (a cycle?)");
            free(k);
            return TA_ERROR_UNSUPPORTED;
        }
        free(k);
    }
    text_put(t, "},\"e\":", 6);
    napi_value event = property(environment, scope, "_event");
    if (wire_value(environment, event, t, 1) != TA_OK) text_put(t, "null", 4);
    text_char(t, '}');
    return t->failed ? TA_ERROR_NO_MEMORY : TA_OK;
}

static ta_status native_serialize(void *d, uint64_t handle, void *buffer, size_t capacity,
                                  size_t *length) {
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    *length = 0;
    if (!instance) return TA_ERROR_INVALID;
    ta_status status =
        scope_wire(system, get_ref(system->environment, instance->scope), &system->wire);
    if (status != TA_OK) return status;
    *length = system->wire.length;
    if (system->wire.length > capacity) return TA_ERROR_NO_MEMORY;
    memcpy(buffer, system->wire.bytes, system->wire.length);
    return TA_OK;
}

/* Copy the enumerable properties of `from` onto `to`. */
static void copy_properties(napi_env environment, napi_value to, napi_value from) {
    if (type_of(environment, from) != napi_object) return;
    napi_value keys;
    napi_get_property_names(environment, from, &keys);
    uint32_t n = array_length(environment, keys);
    for (uint32_t i = 0; i < n; i++) {
        napi_value key, value;
        napi_get_element(environment, keys, i, &key);
        napi_get_property(environment, from, key, &value);
        napi_set_property(environment, to, key, value);
    }
}

static ta_status native_deserialize(void *d, void *context, const ta_datamodel_system_variables *sv,
                                    const void *buffer, size_t length, uint64_t *out) {
    nd_system *system = (nd_system *)d;
    napi_env environment = system->environment;
    napi_value state = value_of_wire(environment, view_of((const char *)buffer, length),
                                     get_ref(environment, system->refs));
    if (!state || type_of(environment, state) != napi_object) return TA_ERROR_INVALID;
    napi_value scope = system_variables_scope(system, sv);
    copy_properties(environment, scope, property(environment, state, "v"));
    napi_value event = property(environment, state, "e");
    if (type_of(environment, event) == napi_object)
        define_hidden(environment, scope, "_event", event, 0);
    instance_new(system, (nd_chart *)context, serial_of(sv->sessionid), scope, out);
    return TA_OK;
}

/* Live migration: the previous bindings are the old variables; those the new
 * chart does not declare go. */
static ta_status native_migrate(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                ta_payload *bindings) {
    ENTER();
    napi_env environment = system->environment;
    napi_value old = get_ref(environment, instance->scope), previous, keys;
    napi_value scope =
        scope_new(system, property(environment, old, "_sessionid"),
                  property(environment, old, "_name"), property(environment, old, "_ioprocessors"));
    napi_create_object(environment, &previous);
    copy_properties(environment, previous, old);
    const ta_image *image = instance->chart->image;
    uint32_t chart = instance->chart->chart, declared = ta_image_data_count(image, chart);
    napi_get_property_names(environment, old, &keys);
    uint32_t n = array_length(environment, keys);
    for (uint32_t k = 0; k < n; k++) {
        napi_value key, v;
        napi_get_element(environment, keys, k, &key);
        size_t kl = 0;
        char *ks = get_string(environment, key, &kl);
        int keep = 0;
        for (uint32_t i = 0; i < declared && !keep && ks; i++) {
            ta_data_info data;
            keep = ta_image_data(image, chart, i, &data) == TA_OK && data.id.length == kl &&
                   !memcmp(data.id.pointer, ks, kl);
        }
        free(ks);
        if (keep) {
            napi_get_property(environment, old, key, &v);
            napi_set_property(environment, scope, key, v);
        }
    }
    set_ref(environment, &instance->scope, scope);
    *bindings = payload_new(system, previous);
    LEAVE(TA_OK);
}

static ta_status native_to_wire(void *d, ta_payload payload, char *buffer, size_t capacity,
                                size_t *length) {
    nd_system *system = (nd_system *)d;
    ta_status status = wire_of(system->environment, payload_get(system, payload), &system->wire);
    if (status != TA_OK) return status;
    *length = system->wire.length;
    if (system->wire.length > capacity) return TA_ERROR_NO_MEMORY;
    memcpy(buffer, system->wire.bytes, system->wire.length);
    return TA_OK;
}

static ta_status native_from_wire(void *d, ta_string_view json, ta_payload *out) {
    nd_system *system = (nd_system *)d;
    napi_value v =
        value_of_wire(system->environment, json, get_ref(system->environment, system->refs));
    if (!v) return TA_ERROR_INVALID;
    *out = payload_new(system, v);
    return TA_OK;
}

static ta_status native_payload_serialize(void *d, ta_payload payload, void *buffer,
                                          size_t capacity, size_t *length) {
    ta_status status = native_to_wire(d, payload, (char *)buffer, capacity, length);
    return status == TA_ERROR_INVALID ? TA_ERROR_UNSUPPORTED : status;
}

static ta_status native_payload_deserialize(void *d, const void *buffer, size_t length,
                                            ta_payload *out) {
    return native_from_wire(d, view_of((const char *)buffer, length), out);
}

static const ta_datamodel_vtable native_vtable = {sizeof(ta_datamodel_vtable),
                                                  0,
                                                  "ecmascript",
                                                  native_attach,
                                                  native_detach,
                                                  native_create,
                                                  native_destroy,
                                                  native_init_data,
                                                  native_bind_event,
                                                  NULL,
                                                  native_condition,
                                                  native_string,
                                                  native_assign,
                                                  native_assign_string,
                                                  native_script,
                                                  native_log,
                                                  native_foreach_begin,
                                                  native_foreach_item,
                                                  native_foreach_end,
                                                  native_capture,
                                                  native_retain,
                                                  native_release,
                                                  NULL,
                                                  native_serialize,
                                                  native_deserialize,
                                                  native_migrate,
                                                  native_to_wire,
                                                  native_from_wire,
                                                  native_payload_serialize,
                                                  native_payload_deserialize};

/* ---- the quickjs "ecmascript" datamodel (src/datamodel_ecmascript), forwarded: the
 * binding only notes which sessions saw an event and keeps their last variables */

static uint32_t map_slot(nd_system *system, uint64_t key) {
    uint32_t i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (system->map_capacity - 1);
    while (system->keys[i] && system->keys[i] != key)
        i = (i + 1) & (system->map_capacity - 1);
    return i;
}

static nd_session *session_at(nd_system *system, uint64_t handle) {
    if (!system->map_capacity || !handle) return NULL;
    uint32_t i = map_slot(system, handle);
    return system->keys[i] == handle && system->sessions[i].image ? &system->sessions[i] : NULL;
}

static void session_put(nd_system *system, uint64_t handle, uint64_t serial, const ta_image *image,
                        uint32_t chart) {
    if ((system->map_count + 1) * 2 > system->map_capacity) {
        uint64_t *keys = system->keys;
        nd_session *sessions = system->sessions;
        uint32_t old = system->map_capacity;
        system->map_capacity = old ? old * 2 : 64;
        system->keys = (uint64_t *)calloc(system->map_capacity, sizeof(uint64_t));
        system->sessions = (nd_session *)calloc(system->map_capacity, sizeof(nd_session));
        if (!system->keys || !system->sessions) abort();
        system->map_count = 0;
        for (uint32_t i = 0; i < old; i++)
            if (keys[i] && sessions[i].image) {
                uint32_t j = map_slot(system, keys[i]);
                system->keys[j] = keys[i];
                system->sessions[j] = sessions[i];
                system->map_count++;
            }
        free(keys);
        free(sessions);
    }
    uint32_t i = map_slot(system, handle);
    if (!system->keys[i]) system->map_count++;
    system->keys[i] = handle;
    system->sessions[i] = (nd_session){serial, image, chart, 0};
}

/* A session's variables as a JS value, read with the C datamodel's eval_json:
 * an object of its <data> ids (undefined ones left out). */
static napi_value quickjs_variables(nd_system *system, uint64_t handle, const ta_image *image,
                                    uint32_t chart) {
    napi_env environment = system->environment;
    text expression = {0};
    text_put(&expression, "({", 2);
    uint32_t n = ta_image_data_count(image, chart);
    for (uint32_t i = 0; i < n; i++) {
        ta_data_info data;
        if (ta_image_data(image, chart, i, &data) != TA_OK) continue;
        if (i) text_char(&expression, ',');
        text_string(&expression, data.id.pointer, data.id.length);
        text_put(&expression, ":typeof ", 8);
        text_put(&expression, data.id.pointer, data.id.length);
        text_put(&expression, "==='undefined'?undefined:", 25);
        text_put(&expression, data.id.pointer, data.id.length);
    }
    text_put(&expression, "})", 2);
    napi_value result = NULL;
    if (!expression.failed) {
        size_t capacity = 4096;
        for (int attempt = 0; attempt < 8 && !result; attempt++) {
            char *buffer = (char *)malloc(capacity);
            if (!buffer) break;
            size_t length = ta_datamodel_ecmascript_eval_json(
                system->quickjs, handle, view_of(expression.bytes, expression.length), buffer,
                capacity);
            if (length == (size_t)-1) {
                free(buffer);
                break;
            }
            if (length < capacity)
                result = value_of_wire(environment, view_of(buffer, length), NULL);
            else
                capacity = length + 1;
            free(buffer);
        }
    }
    free(expression.bytes);
    if (!result) napi_create_object(environment, &result);
    return result;
}

#define Q(system) ((system)->quickjs_vtable)

static ta_status q_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    nd_system *system = (nd_system *)d;
    nd_chart *c = chart_new(chart);
    if (!c) return TA_ERROR_NO_MEMORY;
    ta_status status = Q(system)->chart_attach(system->quickjs, chart, &c->inner);
    if (status != TA_OK) {
        free(c);
        return status;
    }
    chart_add(system, c);
    *context = c;
    return TA_OK;
}

static void q_detach(void *d, void *context) {
    nd_system *system = (nd_system *)d;
    nd_chart *c = (nd_chart *)context;
    if (!c) return;
    Q(system)->chart_detach(system->quickjs, c->inner);
    chart_remove(system, c);
    free(c);
}

static ta_status q_create(void *d, void *context, const ta_datamodel_system_variables *sv,
                          ta_payload init, uint64_t *out) {
    nd_system *system = (nd_system *)d;
    nd_chart *c = (nd_chart *)context;
    ta_status status = Q(system)->create(system->quickjs, c->inner, sv, init, out);
    if (status == TA_OK) {
        session_put(system, *out, serial_of(sv->sessionid), c->image, c->chart);
        nd_session *s = session_at(system, *out);
        if (s) mark_changed(system, &s->stamp, s->serial);
    }
    return status;
}

static void q_destroy(void *d, uint64_t handle) {
    nd_system *system = (nd_system *)d;
    nd_session *s = session_at(system, handle);
    if (s) {
        if (!system->keep_quiet)
            grave_add(system, s->serial, quickjs_variables(system, handle, s->image, s->chart));
        s->image = NULL; /* a tombstone: keeps the probe chains */
    }
    Q(system)->destroy(system->quickjs, handle);
}

static ta_status q_init_data(void *d, uint64_t handle, ta_datamodel_environment *outside,
                             uint32_t declaration) {
    nd_system *system = (nd_system *)d;
    return Q(system)->init_data(system->quickjs, handle, outside, declaration);
}

static ta_status q_bind_event(void *d, uint64_t handle, ta_datamodel_environment *outside,
                              const ta_event_view *event) {
    nd_system *system = (nd_system *)d;
    nd_session *s = session_at(system, handle);
    if (s) mark_changed(system, &s->stamp, s->serial);
    return Q(system)->bind_event(system->quickjs, handle, outside, event);
}

static void q_detach_event(void *d, uint64_t handle) {
    nd_system *system = (nd_system *)d;
    if (Q(system)->detach_event) Q(system)->detach_event(system->quickjs, handle);
}

static ta_status q_condition(void *d, uint64_t handle, ta_datamodel_environment *outside,
                             ta_expression e, int *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->eval_condition(system->quickjs, handle, outside, e, out);
}

static ta_status q_string(void *d, uint64_t handle, ta_datamodel_environment *outside,
                          ta_expression e, ta_string_view *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->eval_string(system->quickjs, handle, outside, e, out);
}

static ta_status q_assign(void *d, uint64_t handle, ta_datamodel_environment *outside,
                          ta_expression location, ta_expression value) {
    nd_system *system = (nd_system *)d;
    return Q(system)->assign(system->quickjs, handle, outside, location, value);
}

static ta_status q_assign_string(void *d, uint64_t handle, ta_expression location,
                                 ta_string_view value) {
    nd_system *system = (nd_system *)d;
    return Q(system)->assign_string(system->quickjs, handle, location, value);
}

static ta_status q_script(void *d, uint64_t handle, ta_datamodel_environment *outside,
                          ta_expression e) {
    nd_system *system = (nd_system *)d;
    return Q(system)->script(system->quickjs, handle, outside, e);
}

static ta_status q_log(void *d, uint64_t handle, ta_datamodel_environment *outside, ta_expression e,
                       ta_string_view *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->log(system->quickjs, handle, outside, e, out);
}

static ta_status q_foreach_begin(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                 ta_expression array, uint64_t *iterator, uint32_t *n) {
    nd_system *system = (nd_system *)d;
    return Q(system)->foreach_begin(system->quickjs, handle, outside, array, iterator, n);
}

static ta_status q_foreach_item(void *d, uint64_t handle, uint64_t iterator, uint32_t i,
                                ta_expression item, ta_expression index) {
    nd_system *system = (nd_system *)d;
    return Q(system)->foreach_item(system->quickjs, handle, iterator, i, item, index);
}

static void q_foreach_end(void *d, uint64_t handle, uint64_t iterator) {
    nd_system *system = (nd_system *)d;
    Q(system)->foreach_end(system->quickjs, handle, iterator);
}

static ta_status q_capture(void *d, uint64_t handle, ta_datamodel_environment *outside,
                           const ta_datamodel_capture *capture, ta_payload *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->capture(system->quickjs, handle, outside, capture, out);
}

static void q_retain(void *d, ta_payload p) {
    nd_system *system = (nd_system *)d;
    Q(system)->retain(system->quickjs, p);
}

static void q_release(void *d, ta_payload p) {
    nd_system *system = (nd_system *)d;
    Q(system)->release(system->quickjs, p);
}

static ta_status q_custom(void *d, uint64_t handle, ta_datamodel_environment *outside,
                          uint32_t custom) {
    nd_system *system = (nd_system *)d;
    return Q(system)->custom ? Q(system)->custom(system->quickjs, handle, outside, custom)
                             : TA_ERROR_UNSUPPORTED;
}

static ta_status q_serialize(void *d, uint64_t handle, void *buffer, size_t capacity,
                             size_t *length) {
    nd_system *system = (nd_system *)d;
    ta_status status = Q(system)->serialize(system->quickjs, handle, buffer, capacity, length);
    if (status == TA_ERROR_UNSUPPORTED)
        snprintf(system->error, sizeof system->error, "%s",
                 ta_datamodel_ecmascript_last_error(system->quickjs));
    return status;
}

static ta_status q_deserialize(void *d, void *context, const ta_datamodel_system_variables *sv,
                               const void *buffer, size_t length, uint64_t *out) {
    nd_system *system = (nd_system *)d;
    nd_chart *c = (nd_chart *)context;
    ta_status status = Q(system)->deserialize(system->quickjs, c->inner, sv, buffer, length, out);
    if (status == TA_OK) session_put(system, *out, serial_of(sv->sessionid), c->image, c->chart);
    return status;
}

static ta_status q_migrate(void *d, uint64_t handle, ta_datamodel_environment *outside,
                           ta_payload *bindings) {
    nd_system *system = (nd_system *)d;
    return Q(system)->migrate(system->quickjs, handle, outside, bindings);
}

static ta_status q_to_wire(void *d, ta_payload p, char *buffer, size_t capacity, size_t *length) {
    nd_system *system = (nd_system *)d;
    return Q(system)->to_wire(system->quickjs, p, buffer, capacity, length);
}

static ta_status q_from_wire(void *d, ta_string_view json, ta_payload *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->from_wire(system->quickjs, json, out);
}

static ta_status q_payload_serialize(void *d, ta_payload p, void *buffer, size_t capacity,
                                     size_t *length) {
    nd_system *system = (nd_system *)d;
    return Q(system)->payload_serialize(system->quickjs, p, buffer, capacity, length);
}

static ta_status q_payload_deserialize(void *d, const void *buffer, size_t length,
                                       ta_payload *out) {
    nd_system *system = (nd_system *)d;
    return Q(system)->payload_deserialize(system->quickjs, buffer, length, out);
}

static const ta_datamodel_vtable quickjs_vtable = {sizeof(ta_datamodel_vtable),
                                                   TA_DATAMODEL_EVENT_DETACHABLE,
                                                   "ecmascript",
                                                   q_attach,
                                                   q_detach,
                                                   q_create,
                                                   q_destroy,
                                                   q_init_data,
                                                   q_bind_event,
                                                   q_detach_event,
                                                   q_condition,
                                                   q_string,
                                                   q_assign,
                                                   q_assign_string,
                                                   q_script,
                                                   q_log,
                                                   q_foreach_begin,
                                                   q_foreach_item,
                                                   q_foreach_end,
                                                   q_capture,
                                                   q_retain,
                                                   q_release,
                                                   q_custom,
                                                   q_serialize,
                                                   q_deserialize,
                                                   q_migrate,
                                                   q_to_wire,
                                                   q_from_wire,
                                                   q_payload_serialize,
                                                   q_payload_deserialize};

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
static ta_string_view wire_of_payload(nd_system *system, ta_payload payload, uint32_t datamodel) {
    system->wire.length = 0;
    if (!payload || datamodel == TA_NIL) return view_of(NULL, 0);
    size_t length = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!text_room(&system->wire, system->wire.capacity ? 0 : 4096)) break;
        ta_status status = system->binding.vtable->to_wire(system, payload, system->wire.bytes,
                                                           system->wire.capacity, &length);
        if (status == TA_OK) return view_of(system->wire.bytes, length);
        if (status != TA_ERROR_NO_MEMORY || !text_room(&system->wire, length + 1)) break;
    }
    return view_of(NULL, 0);
}
static nd_system *get_system(napi_env environment, napi_value v);

static uint64_t trace_wall(nd_system *host) {
    if (!host->clock) return 0;
    napi_value clock = get_ref(host->environment, host->clock), global, r;
    napi_get_global(host->environment, &global);
    double ms = 0;
    if (napi_call_function(host->environment, global, clock, 0, NULL, &r) == napi_ok)
        napi_get_value_double(host->environment, r, &ms);
    return ms > 0 ? (uint64_t)(ms * 1e6) : 0;
}

static uint64_t trace_fuel(nd_system *host) {
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
static void trace_put(nd_system *host) {
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
static void trace_drain(nd_system *host) {
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

/* Attributes given as [[key, value], ...] of strings: ta_otlp_attribute
 * views of texts the caller frees with attributes_free. */
typedef struct js_attributes {
    ta_otlp_attribute *list;
    char **texts;
    uint32_t count;
} js_attributes;

static void attributes_of(napi_env environment, napi_value pairs, js_attributes *out) {
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

static void attributes_free(js_attributes *attributes) {
    for (uint32_t i = 0; i < 2 * attributes->count; i++)
        free(attributes->texts[i]);
    free(attributes->texts);
    free(attributes->list);
}

/* An encoder's resource from [[key, value], ...]. */
static void otlp_resource_set(napi_env environment, ta_otlp *otlp, napi_value pairs) {
    js_attributes attributes;
    attributes_of(environment, pairs, &attributes);
    ta_otlp_set_resource(otlp, attributes.list, attributes.count);
    attributes_free(&attributes);
}

static double named_double(napi_env environment, napi_value object, const char *name);
static uint32_t named_length(napi_env environment, napi_value object, const char *name,
                             napi_value *array);
static uint64_t decimal_of(napi_env environment, napi_value v);

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
static napi_value js_trace_configure(napi_env environment, napi_callback_info info) {
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
static napi_value js_trace_end(napi_env environment, napi_callback_info info) {
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

static double named_double(napi_env environment, napi_value object, const char *name) {
    napi_value v;
    double d = 0;
    if (napi_get_named_property(environment, object, name, &v) == napi_ok)
        napi_get_value_double(environment, v, &d);
    return d;
}

static uint32_t named_length(napi_env environment, napi_value object, const char *name,
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
static napi_value js_metrics(napi_env environment, napi_callback_info info) {
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
static napi_value js_metrics_to_otlp(napi_env environment, napi_callback_info info) {
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
static napi_value js_decode_metrics(napi_env environment, napi_callback_info info) {
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
    for (uint32_t i = 0; i < 2 * IO_PROCESSORS_MAX; i++)
        free(system->io_text[i]);
    set_ref(environment, &system->resolve, NULL);
    set_ref(environment, &system->effects, NULL);
    set_ref(environment, &system->refs, NULL);
    set_ref(environment, &system->clock, NULL);
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

static void system_finalize(napi_env environment, void *data, void *hint) {
    (void)hint;
    nd_system *system = (nd_system *)data;
    system_destroy(environment, system);
    free(system);
}

static nd_system *get_system(napi_env environment, napi_value v) {
    void *p = NULL;
    if (type_of(environment, v) != napi_external ||
        napi_get_value_external(environment, v, &p) != napi_ok || !p) {
        napi_throw_type_error(environment, NULL, "expected a system handle");
        return NULL;
    }
    nd_system *system = (nd_system *)p;
    if (!system->host) {
        napi_throw_error(environment, NULL, "the system was stopped");
        return NULL;
    }
    if (system->running) {
        napi_throw_error(environment, NULL, "the system is running (calls into it do not nest)");
        return NULL;
    }
    system->environment = environment;
    return system;
}

/* For calls that only read (inspect, context): also while effects are being delivered. */
static nd_system *get_system_reading(napi_env environment, napi_value v) {
    void *p = NULL;
    if (type_of(environment, v) == napi_external &&
        napi_get_value_external(environment, v, &p) == napi_ok && p &&
        ((nd_system *)p)->running == 2 && ((nd_system *)p)->host) {
        ((nd_system *)p)->environment = environment;
        return (nd_system *)p;
    }
    return get_system(environment, v);
}

/* ---- the run loop -------------------------------------------------------------------------- */

static napi_value effect_new(nd_system *system, const char *kind) {
    napi_env environment = system->environment;
    napi_value e;
    napi_create_object(environment, &e);
    set(environment, e, "kind", str_value(environment, view_of(kind, strlen(kind))));
    if (!system->effect_list) napi_create_array(environment, &system->effect_list);
    napi_set_element(environment, system->effect_list, system->effect_count++, e);
    return e;
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
        e = effect_new(system, "send");
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
    case TA_EFFECT_WAKE:
        e = effect_new(system, "wake");
        set(environment, e, "session", num(environment, (double)effect->session));
        break;
    case TA_EFFECT_REQUEST:
        answer_request(system, effect);
        break;
    case TA_HOST_EFFECT_MIGRATED:
        e = effect_new(system, "migrated");
        set(environment, e, "session", num(environment, (double)effect->session));
        set(environment, e, "segment", num(environment, effect->segment));
        set(environment, e, "chart", num(environment, effect->chart));
        set(environment, e, "job", num(environment, effect->job));
        break;
    case TA_HOST_EFFECT_JOB_DONE: {
        napi_value left;
        uint32_t k = ta_host_job_pending(system->host, effect->job, NULL, 0);
        uint64_t *buffer = (uint64_t *)malloc(sizeof(uint64_t) * (k ? k : 1));
        k = buffer ? ta_host_job_pending(system->host, effect->job, buffer, k) : 0;
        napi_create_array_with_length(environment, k, &left);
        for (uint32_t j = 0; j < k; j++)
            napi_set_element(environment, left, j, num(environment, (double)buffer[j]));
        free(buffer);
        e = effect_new(system, "job");
        set(environment, e, "job", num(environment, effect->job));
        set(environment, e, "migrated", num(environment, effect->migrated));
        set(environment, e, "failed", num(environment, effect->failed));
        set(environment, e, "pending", left);
        break;
    }
    case TA_HOST_EFFECT_SEGMENT_FREED:
        if (effect->segment < system->image_count)
            set_ref(environment, &system->images[effect->segment], NULL);
        e = effect_new(system, "freed");
        set(environment, e, "segment", num(environment, effect->segment));
        break;
    case TA_HOST_EFFECT_PEER: { /* an envelope for a transport, as neutral JSON */
        char traceparent[TA_TRACEPARENT_LENGTH];
        uint32_t tl = ta_traceparent_format(&effect->trace, traceparent, sizeof traceparent);
        ta_string_view data = wire_of_payload(system, effect->payload, effect->datamodel);
        char *data_copy = data.length ? (char *)malloc(data.length) : NULL;
        if (data_copy) memcpy(data_copy, data.pointer, data.length);
        ta_envelope envelope;
        memset(&envelope, 0, sizeof envelope);
        envelope.size = sizeof envelope;
        envelope.hops = effect->flags;
        envelope.target = effect->target;
        envelope.name = effect->event;
        envelope.sendid = effect->sendid;
        envelope.origin = effect->auxiliary;
        envelope.msgid = effect->msgid;
        envelope.traceparent = view_of(traceparent, tl);
        ta_string_view encoded = view_of(data_copy, data_copy ? data.length : 0);
        size_t length = ta_host_envelope_json(&envelope, encoded, NULL, 0);
        char *json = (char *)malloc(length + 1);
        e = effect_new(system, "peer");
        set(environment, e, "peer", num(environment, effect->peer));
        set(environment, e, "origin", str_value(environment, effect->auxiliary));
        set(environment, e, "msgid", str_value(environment, effect->msgid));
        set(environment, e, "sendid", str_value(environment, effect->sendid));
        set(environment, e, "target", str_value(environment, effect->target));
        if (json) {
            ta_host_envelope_json(&envelope, encoded, json, length + 1);
            set(environment, e, "envelope", str_value(environment, view_of(json, length)));
        }
        if (effect->payload && !data.length)
            set(environment, e, "malformed", bool_value(environment, 1));
        free(json);
        free(data_copy);
        break;
    }
    case TA_HOST_EFFECT_PEER_ERROR:
        e = effect_new(system, "peerError");
        set(environment, e, "peer", num(environment, effect->peer));
        set(environment, e, "origin", str_value(environment, effect->auxiliary));
        set(environment, e, "msgid", str_value(environment, effect->msgid));
        set(environment, e, "sendid", str_value(environment, effect->sendid));
        set(environment, e, "reason", num(environment, effect->token));
        break;
    default:
        break;
    }
}

static uint64_t trace_wall(nd_system *host);
static uint64_t trace_fuel(nd_system *host);
static void trace_drain(nd_system *host);
static void trace_put(nd_system *host);

#define RUN_BUDGET 10000u
#define RUN_ROUNDS 16

/* Run until idle (or RUN_ROUNDS runs); every effect, the changed sessions and
 * the traces go to callbacks.effects at once. Returns the run code. */
static double pump(nd_system *system, double now) {
    napi_env environment = system->environment;
    ta_host_advance_time(system->host, now > 0 ? (ta_time)now : 0);
    system->running = 1;
    system->pump++;
    system->effect_list = NULL;
    system->effect_count = 0;
    system->changed_count = 0;
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
            effect_out(system, &effects[i]);
        ta_host_effects_ack(system->host);
        if (result.status == TA_RUN_IDLE || result.status == TA_RUN_FAULT) break;
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
    napi_value traces = system->traces_length
                            ? bytes_value(environment, system->traces, system->traces_length)
                            : NULL;
    napi_value logs =
        system->logs_length ? bytes_value(environment, system->logs, system->logs_length) : NULL;
    system->traces_length = system->logs_length = 0;
    napi_value list = system->effect_list;
    system->effect_list = NULL;
    system->running = 0;
    if (list || changed || traces || logs) {
        napi_value argv[4] = {
            list ? list : undef(environment), changed ? changed : undef(environment),
            traces ? traces : undef(environment), logs ? logs : undef(environment)};
        system->running = 2; /* the sink may not call in: core.mjs queues */
        napi_value r = call(environment, get_ref(environment, system->effects), 4, argv);
        system->running = 0;
        if (!r) return 0; /* the exception propagates */
    }
    if (result.status != TA_RUN_IDLE) return -2;
    return result.next_deadline == UINT64_MAX ? -1 : (double)result.next_deadline;
}

static napi_value code_value(napi_env environment, double code) {
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    return pending ? NULL : num(environment, code);
}

static double now_of(napi_env environment, napi_value v) {
    return type_of(environment, v) == napi_number ? get_double(environment, v) : 0;
}

/* ---- creating a system ---------------------------------------------------------------------- */

/* config: { systemID, ecmascript: 'native' | 'quickjs', ioProcessors: [{ type, location }],
 * maxActors, maxEvents, maxTimers, stringBytes, seed: [high, low], maxOps, memoryLimit } */
static nd_system *system_new(napi_env environment, napi_value callbacks, napi_value options,
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
    system->refs = make_ref(environment, property(environment, callbacks, "ref"));
    if (type_of(environment, property(environment, callbacks, "clock")) == napi_function)
        system->clock = make_ref(environment, property(environment, callbacks, "clock"));
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

static napi_value system_value(napi_env environment, nd_system *system) {
    napi_value r;
    napi_create_external(environment, system, system_finalize, NULL, &r);
    return r;
}

/* createSystem(callbacks, config) -> system */
static napi_value js_create_system(napi_env environment, napi_callback_info info) {
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

static void keep_image(nd_system *system, uint32_t segment, napi_value image) {
    grow((void **)&system->images, &system->image_capacity, segment + 1, sizeof(napi_ref));
    set_ref(system->environment, &system->images[segment], image);
    if (segment >= system->image_count) system->image_count = segment + 1;
}

/* addImage(system, image) -> segment: its charts become the current versions */
static napi_value js_add_image(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    nd_image *image = system ? get_image(environment, argv[1]) : NULL;
    if (!image) return NULL;
    uint32_t segment = 0;
    system->running = 1; /* charts attach: resolve() runs JavaScript */
    ta_status status = ta_host_add_image(system->host, &image->image, &segment);
    system->running = 0;
    if (status != TA_OK) {
        napi_throw_error(environment, NULL, "cannot add the image");
        return NULL;
    }
    keep_image(system, segment, argv[1]);
    return num(environment, segment);
}

/* chart(system, name) -> [segment, chart] of the current version, or null */
static napi_value js_chart(napi_env environment, napi_callback_info info) {
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

static napi_value js_boot(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    system->running = 1;
    ta_host_advance_time(system->host, argc > 2 ? (ta_time)now_of(environment, argv[2]) : 0);
    ta_status status = ta_host_boot(system->host, get_u32(environment, argv[1]));
    system->running = 0;
    if (status != TA_OK) {
        napi_throw_error(environment, NULL, "cannot boot the image");
        return NULL;
    }
    return undef(environment);
}

static napi_value js_actor_by_name(napi_env environment, napi_callback_info info) {
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
static napi_value js_actors(napi_env environment, napi_callback_info info) {
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

static char *optional_string(napi_env environment, napi_value v, size_t *n) {
    *n = 0;
    return type_of(environment, v) == napi_string ? get_string(environment, v, n) : NULL;
}

/* spawn(system, segment, chart, name, input, traceparent, now) -> session (the
 * bootstrap runs with the next pump) */
static napi_value js_spawn(napi_env environment, napi_callback_info info) {
    size_t argc = 7;
    napi_value argv[7];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 7 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    ta_payload input = payload_of_value(system, argv[4]);
    bool pending = false;
    napi_is_exception_pending(environment, &pending);
    if (pending) return NULL;
    size_t nl = 0, tl = 0;
    char *name = optional_string(environment, argv[3], &nl),
         *traceparent = optional_string(environment, argv[5], &tl);
    uint64_t session = 0;
    ta_host_advance_time(system->host, (ta_time)now_of(environment, argv[6]));
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
    return num(environment, (double)session);
}

/* send(system, session, name, data, traceparent, now) -> run code: one event, then the run loop */
static napi_value js_send(napi_env environment, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 6 ? get_system(environment, argv[0]) : NULL;
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
    return code_value(environment, pump(system, now_of(environment, argv[5])));
}

/* sendMany(system, sessions, names, datas, traceparents, now) -> [accepted, run code] */
static napi_value js_send_many(napi_env environment, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 6 ? get_system(environment, argv[0]) : NULL;
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
    double code = pump(system, now_of(environment, argv[5]));
    if (!code_value(environment, code)) return NULL;
    napi_value r;
    napi_create_array_with_length(environment, 2, &r);
    napi_set_element(environment, r, 0, num(environment, delivered));
    napi_set_element(environment, r, 1, num(environment, code));
    return r;
}

/* post(system, session, { name, data, wire, sendid, origin, origintype, raw, traceparent,
 * flags, token }, now) -> run code: an event with every field of ta_event_in */
static napi_value js_post(napi_env environment, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 4 ? get_system(environment, argv[0]) : NULL;
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
    return code_value(environment, pump(system, now_of(environment, argv[3])));
}

/* deliver(system, peer, { target, name, data | wire, origin, origintype, sendid, msgid,
 * dedup, hops, traceparent }, now) -> outcome (TA_ROUTE_*), -1 if it failed */
static napi_value js_deliver(napi_env environment, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 4 ? get_system(environment, argv[0]) : NULL;
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
    ta_host_advance_time(system->host, (ta_time)now_of(environment, argv[3]));
    ta_status status =
        pending ? TA_ERROR_INVALID
                : ta_host_deliver(system->host, get_u32(environment, argv[1]), &envelope, &outcome);
    if (status != TA_OK) payload_drop(system, envelope.data);
    for (int i = 0; i < 9; i++)
        free(texts[i]);
    if (pending) return NULL;
    double code = pump(system, now_of(environment, argv[3]));
    if (!code_value(environment, code)) return NULL;
    napi_value r;
    napi_create_array_with_length(environment, 2, &r);
    napi_set_element(environment, r, 0, num(environment, status == TA_OK ? (double)outcome : -1));
    napi_set_element(environment, r, 1, num(environment, code));
    return r;
}

/* pump(system, now) -> run code */
static napi_value js_pump(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    return code_value(environment, pump(system, now_of(environment, argv[1])));
}

static napi_value js_terminate(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    return bool_value(environment,
                      ta_host_terminate(system->host, get_session(environment, argv[1])) == TA_OK);
}

static int locate(nd_system *system, uint64_t session, ta_system **segment_system,
                  uint32_t *segment, ta_actor_info *info) {
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
static napi_value js_inspect(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], o, states;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system_reading(environment, argv[0]) : NULL;
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

/* context(system, session) -> the session's variables (native: its scope) */
static napi_value js_context(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system_reading(environment, argv[0]) : NULL;
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

/* ---- hibernation, moving actors, migration -------------------------------------------------- */

static napi_value js_sweep(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], out;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    uint32_t max = get_u32(environment, argv[1]);
    if (!max) max = 1u << 30;
    uint32_t capacity = max < 65536 ? max : 65536;
    uint64_t *sessions = (uint64_t *)malloc(sizeof(uint64_t) * capacity);
    uint32_t n = sessions ? ta_host_sweep(system->host, max, sessions, capacity) : 0;
    napi_create_array_with_length(environment, n, &out);
    for (uint32_t i = 0; i < n; i++)
        napi_set_element(environment, out, i, num(environment, (double)sessions[i]));
    free(sessions);
    return out;
}

static void throw_status(napi_env environment, const char *what, ta_status status,
                         const char *detail) {
    char message[700];
    const char *why = status == TA_ERROR_STATE
                          ? "the actor is not at a stable point (busy, invoking, or queued events)"
                      : status == TA_ERROR_UNSUPPORTED ? detail
                      : status == TA_ERROR_INVALID
                          ? "no such actor, or the blob does not belong here"
                      : status == TA_ERROR_NO_MEMORY ? "out of memory"
                                                     : "failed";
    snprintf(message, sizeof message, "%s: %s", what, why && *why ? why : "cannot be stored");
    napi_throw_error(environment, NULL, message);
}

/* A blob through a call that sizes it first (*length = needed on TA_ERROR_NO_MEMORY). */
typedef ta_status (*blob_writer)(nd_system *system, uint64_t session, void *buffer, size_t capacity,
                                 size_t *length);

static ta_status write_hibernate(nd_system *system, uint64_t session, void *b, size_t c,
                                 size_t *l) {
    return ta_host_hibernate(system->host, session, b, c, l);
}

static ta_status write_export(nd_system *system, uint64_t session, void *b, size_t c, size_t *l) {
    return ta_host_export_actor(system->host, session, b, c, l);
}

static ta_status write_blob(nd_system *system, blob_writer writer, uint64_t session, text *out) {
    size_t length = 0;
    ta_status status = TA_ERROR_NO_MEMORY;
    system->error[0] = 0;
    system->keep_quiet = 1;
    system->running = 1;
    for (int attempt = 0; attempt < 8 && status == TA_ERROR_NO_MEMORY; attempt++) {
        if (!text_room(out, length > out->capacity ? length : 4096)) break;
        status = writer(system, session, out->bytes, out->capacity, &length);
    }
    system->running = 0;
    system->keep_quiet = 0;
    out->length = status == TA_OK ? length : 0;
    return status;
}

/* hibernate(system, session) -> blob; throws (saying why) when it cannot */
static napi_value js_hibernate(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    text blob = {0};
    ta_status status =
        write_blob(system, write_hibernate, get_session(environment, argv[1]), &blob);
    napi_value r = status == TA_OK ? bytes_value(environment, blob.bytes, blob.length) : NULL;
    if (status != TA_OK)
        throw_status(environment, "cannot hibernate the actor", status, system->error);
    free(blob.bytes);
    return r;
}

/* wake(system, session, blob): the actor is back; its work runs with the next pump */
static napi_value js_wake(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    void *data = NULL;
    size_t n = 0;
    if (!system) return NULL;
    if (!bytes_of(environment, argv[2], &data, &n)) {
        napi_throw_type_error(environment, NULL, "wake(system, session, blob: Uint8Array)");
        return NULL;
    }
    void *aligned = malloc(n ? n : 1); /* the blob is read in place */
    if (!aligned) abort();
    memcpy(aligned, data, n);
    system->running = 1;
    ta_status status = ta_host_wake(system->host, get_session(environment, argv[1]), aligned, n);
    system->running = 0;
    free(aligned);
    if (status != TA_OK) {
        throw_status(environment, "cannot wake the actor", status, NULL);
        return NULL;
    }
    return undef(environment);
}

/* exportActor(system, session, now) -> blob: the actor leaves; its delayed sends
 * are due relative to time 0 (ta_blob_rebase), so any clock adopts it */
static napi_value js_export_actor(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    text blob = {0};
    ta_host_advance_time(system->host, (ta_time)now_of(environment, argv[2]));
    ta_status status = write_blob(system, write_export, get_session(environment, argv[1]), &blob);
    if (status == TA_OK)
        status = ta_blob_rebase(blob.bytes, blob.length, (ta_time)now_of(environment, argv[2]), 0);
    napi_value r = status == TA_OK ? bytes_value(environment, blob.bytes, blob.length) : NULL;
    if (status != TA_OK)
        throw_status(environment, "cannot export the actor", status, system->error);
    free(blob.bytes);
    return r;
}

/* adopt(system, blob, now) -> [session, run code]: into the newest segment whose image the
 * blob belongs to */
static napi_value js_adopt(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3], r;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    void *data = NULL;
    size_t n = 0;
    if (!system) return NULL;
    if (!bytes_of(environment, argv[1], &data, &n)) {
        napi_throw_type_error(environment, NULL, "adopt(system, blob: Uint8Array, now)");
        return NULL;
    }
    void *copy = malloc(n ? n : 1);
    if (!copy) abort();
    memcpy(copy, data, n);
    ta_time now = (ta_time)now_of(environment, argv[2]);
    ta_status status = ta_blob_rebase(copy, n, 0, now);
    uint64_t session = 0;
    ta_host_advance_time(system->host, now);
    system->running = 1;
    for (uint32_t k = ta_host_segment_count(system->host); status == TA_OK && k-- > 0;) {
        ta_host_segment_info segment_info;
        segment_info.size = sizeof segment_info;
        if (ta_host_segment_info_get(system->host, k, &segment_info) != TA_OK || !segment_info.live)
            continue;
        status = ta_host_adopt(system->host, k, copy, n, &session);
        if (status != TA_ERROR_INVALID) break;
        status = TA_OK;
    }
    system->running = 0;
    free(copy);
    if (status != TA_OK || !session) {
        throw_status(environment, "cannot adopt the actor",
                     status == TA_OK ? TA_ERROR_INVALID : status, "");
        return NULL;
    }
    double code = pump(system, (double)now);
    if (!code_value(environment, code)) return NULL;
    napi_create_array_with_length(environment, 2, &r);
    napi_set_element(environment, r, 0, num(environment, (double)session));
    napi_set_element(environment, r, 1, num(environment, code));
    return r;
}

/* migrate(system, name, session, wake, timeout) -> job */
static napi_value js_migrate(napi_env environment, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 5 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    size_t nl = 0;
    char *name = optional_string(environment, argv[1], &nl);
    bool wake = false;
    napi_get_value_bool(environment, argv[3], &wake);
    uint32_t job = 0;
    ta_status status = ta_host_migrate(
        system->host, view_of(name, nl), get_session(environment, argv[2]),
        wake ? TA_HOST_MIGRATE_WAKE : 0, (ta_time)get_double(environment, argv[4]), &job);
    free(name);
    if (status != TA_OK) {
        napi_throw_error(environment, NULL, "cannot start the migration");
        return NULL;
    }
    return num(environment, job);
}

/* ---- snapshots ------------------------------------------------------------------------------ */

/* The native datamodel's tables as neutral JSON: {"i": [[handle, serial, hash,
 * chart, state], ...], "p": [[handle, refs, value], ...]} */
static ta_status native_snapshot(nd_system *system, text *t) {
    napi_env environment = system->environment;
    text state = {0};
    t->length = 0;
    text_put(t, "{\"i\":[", 6);
    int first = 1;
    for (uint32_t i = 0; i < system->instance_count; i++) {
        nd_instance *instance = &system->instances[i];
        if (!instance->live) continue;
        ta_status status = scope_wire(system, get_ref(environment, instance->scope), &state);
        if (status != TA_OK) {
            free(state.bytes);
            return status;
        }
        char head[96];
        int n = snprintf(head, sizeof head, "%s[%u,\"%llu\",\"%016llx\",%u,", first ? "" : ",",
                         i + 1, (unsigned long long)instance->serial,
                         (unsigned long long)instance->chart->hash, instance->chart->chart);
        first = 0;
        text_put(t, head, (size_t)n);
        text_put(t, state.bytes, state.length);
        text_char(t, ']');
    }
    free(state.bytes);
    text_put(t, "],\"p\":[", 7);
    first = 1;
    for (uint32_t i = 0; i < system->payload_count; i++) {
        if (!system->payload_refs[i]) continue;
        char head[48];
        int n = snprintf(head, sizeof head, "%s[%u,%u,", first ? "" : ",", i + 1,
                         system->payload_refs[i]);
        first = 0;
        text_put(t, head, (size_t)n);
        if (wire_value(environment, get_ref(environment, system->payloads[i]), t, 1) != TA_OK) {
            snprintf(system->error, sizeof system->error,
                     "a queued event's data is not a neutral value");
            return TA_ERROR_UNSUPPORTED;
        }
        text_char(t, ']');
    }
    text_put(t, "]}", 2);
    return t->failed ? TA_ERROR_NO_MEMORY : TA_OK;
}

/* snapshot(system) -> { host, datamodel, segments: [image | null] } (quiescent: no migration) */
static napi_value js_snapshot(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out, segments;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *system = argc >= 1 ? get_system(environment, argv[0]) : NULL;
    if (!system) return NULL;
    size_t length = 0;
    ta_status status = ta_host_snapshot(system->host, NULL, 0, &length);
    void *host =
        status == TA_OK || status == TA_ERROR_NO_MEMORY ? malloc(length ? length : 1) : NULL;
    if (host) status = ta_host_snapshot(system->host, host, length, &length);
    if (!host || status != TA_OK) {
        free(host);
        throw_status(environment, "snapshot", status == TA_OK ? TA_ERROR_NO_MEMORY : status, "");
        return NULL;
    }
    text datamodel = {0};
    system->error[0] = 0;
    if (system->native) {
        status = native_snapshot(system, &datamodel);
    } else {
        size_t dl = 0;
        status = ta_datamodel_ecmascript_snapshot(system->quickjs, NULL, 0, &dl);
        if ((status == TA_OK || status == TA_ERROR_NO_MEMORY) && text_room(&datamodel, dl + 1))
            status = ta_datamodel_ecmascript_snapshot(system->quickjs, datamodel.bytes,
                                                      datamodel.capacity, &dl);
        datamodel.length = dl;
        if (status == TA_ERROR_UNSUPPORTED)
            snprintf(system->error, sizeof system->error, "%s",
                     ta_datamodel_ecmascript_last_error(system->quickjs));
    }
    if (status != TA_OK) {
        free(host);
        free(datamodel.bytes);
        throw_status(environment, "snapshot", status, system->error);
        return NULL;
    }
    napi_create_object(environment, &out);
    set(environment, out, "host", bytes_value(environment, host, length));
    set(environment, out, "datamodel", bytes_value(environment, datamodel.bytes, datamodel.length));
    free(host);
    free(datamodel.bytes);
    uint32_t count = ta_host_segment_count(system->host);
    napi_create_array_with_length(environment, count, &segments);
    for (uint32_t k = 0; k < count; k++) {
        ta_host_segment_info segment_info;
        segment_info.size = sizeof segment_info;
        int live = ta_host_segment_info_get(system->host, k, &segment_info) == TA_OK &&
                   segment_info.live && k < system->image_count && system->images[k];
        napi_set_element(environment, segments, k,
                         live ? get_ref(environment, system->images[k]) : null_value(environment));
    }
    set(environment, out, "segments", segments);
    return out;
}

static nd_chart *chart_by_hash(nd_system *system, uint64_t hash, uint32_t chart) {
    for (uint32_t i = 0; i < system->chart_count; i++)
        if (system->charts[i]->hash == hash && system->charts[i]->chart == chart)
            return system->charts[i];
    return NULL;
}

static uint64_t hex_of(napi_env environment, napi_value v) {
    size_t n = 0;
    char *t = get_string(environment, v, &n);
    uint64_t high = 0, low = 0;
    if (!t || !hex_text(t, n, &high, &low)) low = 0;
    free(t);
    return low;
}

/* The native datamodel's tables back at their handles. */
static ta_status native_restore(nd_system *system, ta_string_view json) {
    napi_env environment = system->environment;
    napi_value tables = value_of_wire(environment, json, get_ref(environment, system->refs));
    if (!tables) return TA_ERROR_INVALID;
    napi_value instances = property(environment, tables, "i"),
               payloads = property(environment, tables, "p");
    uint32_t n = array_length(environment, instances), max = 0;
    for (uint32_t i = 0; i < n; i++) {
        napi_value row, v;
        napi_get_element(environment, instances, i, &row);
        napi_get_element(environment, row, 0, &v);
        uint32_t handle = get_u32(environment, v);
        if (handle > max) max = handle;
    }
    grow((void **)&system->instances, &system->instance_capacity, max, sizeof(nd_instance));
    system->instance_count = max;
    for (uint32_t i = 0; i < n; i++) {
        napi_value row, handle, serial, hash, chart, state;
        napi_get_element(environment, instances, i, &row);
        napi_get_element(environment, row, 0, &handle);
        napi_get_element(environment, row, 1, &serial);
        napi_get_element(environment, row, 2, &hash);
        napi_get_element(environment, row, 3, &chart);
        napi_get_element(environment, row, 4, &state);
        nd_chart *c = chart_by_hash(system, hex_of(environment, hash), get_u32(environment, chart));
        if (!c) return TA_ERROR_INVALID;
        uint32_t h = get_u32(environment, handle);
        nd_instance *instance = &system->instances[h - 1];
        memset(instance, 0, sizeof *instance);
        instance->live = 1;
        instance->chart = c;
        instance->serial = decimal_of(environment, serial);
        char sessionid[24];
        snprintf(sessionid, sizeof sessionid, "%llu", (unsigned long long)instance->serial);
        napi_value io;
        napi_create_object(environment, &io);
        for (uint32_t k = 0; k < system->io_count; k++) { /* as the system advertises them */
            napi_value entry;
            napi_create_object(environment, &entry);
            set(environment, entry, "location", str_value(environment, system->io[k].location));
            napi_set_property(environment, io, str_value(environment, system->io[k].type), entry);
        }
        napi_value scope =
            scope_new(system, str_value(environment, view_of(sessionid, strlen(sessionid))),
                      undef(environment), io);
        copy_properties(environment, scope, property(environment, state, "v"));
        napi_value event = property(environment, state, "e");
        if (type_of(environment, event) == napi_object)
            define_hidden(environment, scope, "_event", event, 0);
        instance->scope = make_ref(environment, scope);
        system->live_instances++;
    }
    system->free_instance = 0;
    for (uint32_t i = max; i-- > 0;)
        if (!system->instances[i].live) {
            system->instances[i].next_free = system->free_instance;
            system->free_instance = i + 1;
        }
    n = array_length(environment, payloads);
    max = 0;
    for (uint32_t i = 0; i < n; i++) {
        napi_value row, v;
        napi_get_element(environment, payloads, i, &row);
        napi_get_element(environment, row, 0, &v);
        if (get_u32(environment, v) > max) max = get_u32(environment, v);
    }
    uint32_t c = system->payload_capacity;
    grow((void **)&system->payloads, &c, max, sizeof(napi_ref));
    grow((void **)&system->payload_refs, &system->payload_capacity, max, sizeof(uint32_t));
    system->payload_count = max;
    for (uint32_t i = 0; i < n; i++) {
        napi_value row, handle, refs, value;
        napi_get_element(environment, payloads, i, &row);
        napi_get_element(environment, row, 0, &handle);
        napi_get_element(environment, row, 1, &refs);
        napi_get_element(environment, row, 2, &value);
        uint32_t h = get_u32(environment, handle);
        system->payloads[h - 1] = make_ref(environment, value);
        system->payload_refs[h - 1] = get_u32(environment, refs);
        system->live_payloads++;
    }
    system->free_payload = 0;
    for (uint32_t i = max; i-- > 0;)
        if (!system->payload_refs[i]) {
            system->payloads[i] = (napi_ref)(uintptr_t)system->free_payload;
            system->free_payload = i + 1;
        }
    return TA_OK;
}

/* restoreSystem(callbacks, config, images: [image | null], host, datamodel) -> system */
static napi_value js_restore_system(napi_env environment, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *host = NULL, *datamodel = NULL;
    size_t hl = 0, dl = 0;
    if (argc < 5 || !bytes_of(environment, argv[3], &host, &hl) ||
        !bytes_of(environment, argv[4], &datamodel, &dl)) {
        napi_throw_type_error(environment, NULL,
                              "restoreSystem(callbacks, config, images, host, datamodel)");
        return NULL;
    }
    uint32_t n = array_length(environment, argv[2]);
    void *aligned = malloc(hl ? hl : 1); /* the snapshot's records are read in place */
    if (!aligned) abort();
    memcpy(aligned, host, hl);
    /* the images it needs: one per segment, by content hash (0: retired) */
    uint32_t segments = ta_host_snapshot_segments(aligned, hl, NULL, 0);
    uint64_t *hashes = (uint64_t *)calloc(segments + 1, sizeof(uint64_t));
    if (segments) ta_host_snapshot_segments(aligned, hl, hashes, segments);
    for (uint32_t k = 0; k < segments; k++) {
        napi_value v = NULL;
        nd_image *image = NULL;
        ta_image_info image_info;
        image_info.size = sizeof image_info;
        if (k < n) napi_get_element(environment, argv[2], k, &v);
        if (type_of(environment, v) == napi_external) {
            napi_get_value_external(environment, v, (void **)&image);
            if (image) ta_image_get_info(&image->image, &image_info);
        }
        if (hashes[k] && (!image || image_info.content_hash != hashes[k])) {
            char message[160];
            snprintf(
                message, sizeof message,
                "restore: segment %u ran the image %016llx, which is not among the images given", k,
                (unsigned long long)hashes[k]);
            free(hashes);
            free(aligned);
            napi_throw_error(environment, NULL, message);
            return NULL;
        }
    }
    free(hashes);
    if (!segments) {
        free(aligned);
        napi_throw_type_error(environment, NULL, "restore: not a host snapshot");
        return NULL;
    }
    ta_host_config config;
    nd_system *system = system_new(environment, argv[0], argv[1], &config);
    if (!system) {
        free(aligned);
        return NULL;
    }
    const ta_image **images = (const ta_image **)calloc(n + 1, sizeof *images);
    for (uint32_t k = 0; k < n; k++) {
        napi_value v;
        napi_get_element(environment, argv[2], k, &v);
        if (type_of(environment, v) != napi_external) continue;
        nd_image *image = get_image(environment, v);
        if (!image) break;
        images[k] = &image->image;
        keep_image(system, k, v);
    }
    system->running = 1;
    ta_status status = ta_host_restore(&config, images, n, aligned, hl, &system->host);
    free(aligned);
    const char *step = "the host's snapshot does not match these images or this build";
    if (status == TA_OK) {
        step = "the datamodel's snapshot does not match";
        if (system->native) {
            status = native_restore(system, view_of((const char *)datamodel, dl));
        } else {
            status = ta_datamodel_ecmascript_restore(system->quickjs, datamodel, dl);
            uint64_t cursor = 0, batch[256]; /* what the binding knows of each session */
            uint32_t count;
            while (status == TA_OK &&
                   (count = ta_host_actors(system->host, &cursor, batch, 256)) > 0)
                for (uint32_t i = 0; i < count; i++) {
                    ta_system *segment_system;
                    uint32_t segment = 0;
                    ta_actor_info actor_info;
                    ta_host_segment_info segment_info;
                    segment_info.size = sizeof segment_info;
                    if (locate(system, batch[i], &segment_system, &segment, &actor_info) &&
                        actor_info.datamodel_instance &&
                        ta_host_segment_info_get(system->host, segment, &segment_info) == TA_OK)
                        session_put(system, actor_info.datamodel_instance, actor_info.serial,
                                    segment_info.image, actor_info.chart);
                }
        }
    }
    system->running = 0;
    free(images);
    if (status != TA_OK) {
        system_finalize(environment, system, NULL);
        char message[160];
        snprintf(message, sizeof message, "restore: %s (%d)", step, status);
        napi_throw_error(environment, NULL, message);
        return NULL;
    }
    return system_value(environment, system);
}

/* ---- inspection -------------------------------------------------------------------------------
 */

static void set_u64(napi_env environment, napi_value o, const char *name, uint64_t v) {
    set(environment, o, name, num(environment, (double)v));
}

/* stats(system) -> { actors, hibernated, runnable, segments, jobs, regionBytes, delivered,
 * routed, migrated, retired, sessions, payloads, charts: [...], segments: [...] } */
static napi_value js_stats(napi_env environment, napi_callback_info info) {
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

static napi_value js_destroy(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *p = NULL;
    if (!argc || napi_get_value_external(environment, argv[0], &p) != napi_ok || !p)
        return undef(environment);
    nd_system *system = (nd_system *)p;
    if (system->running) {
        napi_throw_error(environment, NULL, "the system is running");
        return NULL;
    }
    system_destroy(environment, system);
    return undef(environment);
}
/* an array of strings as ta_string_view views (free with strs_free) */
static ta_string_view *get_strs(napi_env environment, napi_value arr, uint32_t *n) {
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

static void strs_free(ta_string_view *s, uint32_t n) {
    for (uint32_t i = 0; s && i < n; i++)
        if (s[i].length) free((void *)s[i].pointer);
    free(s);
}

/* peerAdd(host, id, reach[], flags, maxQueue, queueTTLMs) -> peer handle (0: refused) */
static napi_value js_peer_add(napi_env environment, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 4 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t il = 0;
    char *id = get_string(environment, argv[1], &il);
    uint32_t nr = 0;
    ta_string_view *reach = get_strs(environment, argv[2], &nr);
    ta_peer_config c;
    memset(&c, 0, sizeof c);
    c.size = sizeof c;
    c.flags = get_u32(environment, argv[3]);
    c.id = (ta_string_view){id ? id : "", (uint32_t)il};
    c.reach = reach;
    c.reach_count = nr;
    c.max_queue = argc > 4 ? get_u32(environment, argv[4]) : 0;
    c.queue_ttl = argc > 5 ? (ta_time)get_session(environment, argv[5]) : 0;
    uint32_t peer = 0;
    if (id && ta_host_peer_add(host->host, &c, &peer) != TA_OK) peer = 0;
    free(id);
    strs_free(reach, nr);
    return num(environment, peer);
}

/* peerUp(host, peer, up) -> bool */
static napi_value js_peer_up(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    bool up = false;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    napi_get_value_bool(environment, argv[2], &up);
    return bool_value(environment,
                      ta_host_peer_up(host->host, get_u32(environment, argv[1]), up) == TA_OK);
}

/* peerRemove(host, peer) -> bool */
static napi_value js_peer_remove(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    return bool_value(environment,
                      ta_host_peer_remove(host->host, get_u32(environment, argv[1])) == TA_OK);
}

/* peerReach(host, peer, reach[]) -> bool */
static napi_value js_peer_reach(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    uint32_t nr = 0;
    ta_string_view *reach = get_strs(environment, argv[2], &nr);
    ta_status status = ta_host_peer_reach(host->host, get_u32(environment, argv[1]), reach, nr);
    strs_free(reach, nr);
    return bool_value(environment, status == TA_OK);
}

/* route(host, systemID, peer) -> bool (peer 0: remove the route) */
static napi_value js_route(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t l = 0;
    char *system = get_string(environment, argv[1], &l);
    ta_status status = system ? ta_host_route(host->host, (ta_string_view){system, (uint32_t)l},
                                              get_u32(environment, argv[2]))
                              : TA_ERROR_INVALID;
    free(system);
    return bool_value(environment, status == TA_OK);
}

/* exportName(host, peer, name, events) -> bool: policy for untrusted peers */
static napi_value js_export(napi_env environment, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 4 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t nl = 0, el = 0;
    char *name = get_string(environment, argv[2], &nl);
    char *events = get_string(environment, argv[3], &el);
    ta_status status = name && events ? ta_host_export(host->host, get_u32(environment, argv[1]),
                                                       (ta_string_view){name, (uint32_t)nl},
                                                       (ta_string_view){events, (uint32_t)el})
                                      : TA_ERROR_INVALID;
    free(name);
    free(events);
    return bool_value(environment, status == TA_OK);
}

/* peerError(host, peer, origin, msgid, sendid, reason) -> bool: an `err` frame */
static napi_value js_peer_error(napi_env environment, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 6 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t ol = 0, ml = 0, sl = 0;
    char *o = get_string(environment, argv[2], &ol), *m = get_string(environment, argv[3], &ml),
         *sd = get_string(environment, argv[4], &sl);
    ta_status status = ta_host_peer_error(host->host, get_u32(environment, argv[1]),
                                          (ta_string_view){o ? o : "", o ? (uint32_t)ol : 0},
                                          (ta_string_view){m ? m : "", m ? (uint32_t)ml : 0},
                                          (ta_string_view){sd ? sd : "", sd ? (uint32_t)sl : 0},
                                          get_u32(environment, argv[5]));
    free(o);
    free(m);
    free(sd);
    return bool_value(environment, status == TA_OK);
}

/* peerRtt(host, peer, ms) */
static napi_value js_peer_round_trip_time(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 3 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    ta_host_peer_round_trip_time(host->host, get_u32(environment, argv[1]),
                                 get_u32(environment, argv[2]));
    return undef(environment);
}

/* peerStatus(host, peer) -> { id, state, queued, rtt, lag, sent, received, errors,
 * duplicates } | null */
static napi_value js_peer_status(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], o;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_system *host = argc >= 2 ? get_system(environment, argv[0]) : NULL;
    if (!host) return NULL;
    ta_peer_status status;
    memset(&status, 0, sizeof status);
    status.size = sizeof status;
    if (ta_host_peer_status(host->host, get_u32(environment, argv[1]), &status) != TA_OK)
        return null_value(environment);
    static const char *const states[] = {"connecting", "open", "reconnecting", "closed"};
    napi_create_object(environment, &o);
    napi_set_named_property(environment, o, "id", str_value(environment, status.id));
    napi_set_named_property(
        environment, o, "state",
        str_value(environment, (ta_string_view){states[status.state & 3],
                                                (uint32_t)strlen(states[status.state & 3])}));
    napi_set_named_property(environment, o, "queued", num(environment, status.queued));
    napi_set_named_property(environment, o, "rtt", num(environment, status.round_trip_time));
    napi_set_named_property(environment, o, "lag", num(environment, (double)status.lag));
    napi_set_named_property(environment, o, "sent", num(environment, (double)status.sent));
    napi_set_named_property(environment, o, "received", num(environment, (double)status.received));
    napi_set_named_property(environment, o, "errors", num(environment, (double)status.errors));
    napi_set_named_property(environment, o, "duplicates",
                            num(environment, (double)status.duplicates));
    return o;
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
        METHOD("terminate", js_terminate),
        METHOD("inspect", js_inspect),
        METHOD("context", js_context),
        METHOD("sweep", js_sweep),
        METHOD("hibernate", js_hibernate),
        METHOD("wake", js_wake),
        METHOD("exportActor", js_export_actor),
        METHOD("adopt", js_adopt),
        METHOD("migrate", js_migrate),
        METHOD("snapshot", js_snapshot),
        METHOD("stats", js_stats),
        METHOD("peerAdd", js_peer_add),
        METHOD("peerUp", js_peer_up),
        METHOD("peerRemove", js_peer_remove),
        METHOD("peerReach", js_peer_reach),
        METHOD("route", js_route),
        METHOD("exportName", js_export),
        METHOD("peerError", js_peer_error),
        METHOD("peerRoundTripTime", js_peer_round_trip_time),
        METHOD("peerStatus", js_peer_status),
        METHOD("traceConfigure", js_trace_configure),
        METHOD("traceEnd", js_trace_end),
        METHOD("metrics", js_metrics),
        METHOD("destroy", js_destroy),
    };
    napi_define_properties(environment, exports, sizeof props / sizeof props[0], props);
    set(environment, exports, "constants", constants_value(environment));
    return exports;
}
