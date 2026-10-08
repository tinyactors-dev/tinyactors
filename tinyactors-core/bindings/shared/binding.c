/* binding.c — the shared JavaScript binding core.
 *
 * The synchronous Node-API value operations are the adapter seam: Node uses
 * its native implementation; web/src/node_api.h and web/native.mjs implement
 * only this subset over wasm imports. Neither adapter knows about actors.
 *
 * Procedural core used by bindings/shared/core.mjs. One JS `System` owns one
 * host: the "xstate" datamodel (below) and a ta_host (include/tinyactors/host.h)
 * that runs every actor of the System — segments sharing the base URI
 * "tinyactors://<id>/", sends between them, versions (reload), migration jobs,
 * the hibernation sweep — so this file only marshals. Actors are named by
 * their session id (a double: exact below 2^53) for their whole life.
 *
 *   compile([{name, xml}]) -> image           chartIndex(image, source) -> chart or -1
 *   createHost(resolver, codec, base) -> host addImage(host, image) -> segment
 *   chart(host, name) -> [segment, chart]|null  (the current version of a chart name)
 *   spawn(host, segment, chart, input) -> session
 *   send(host, session, type, event) -> bool  sendMany(host, sessions, types, events) -> n
 *   deliver(host, session, type, data, origin) -> bool   (from another system)
 *   run(host, budget) -> { status, deadline, used, logs, done, ended, sends, errors, wakes,
 *                          requests, migrated, jobs, freed }
 *   advance(host, ms) / config(host, session) / context(host, session) / terminate(...)
 *   stats(host) / destroy(host)
 *   sweep(host, max) -> sessions              idle since the last sweep, hibernatable
 *   hibernate(host, session) -> Uint8Array|null   wake(host, session, blob) -> bool
 *   migrate(host, name, session, wake, timeoutMs) -> job   (run() reports `migrated`, `jobs`)
 *   failSend(host, session, sendid)           resume(host, segment, request, chart|-1)
 *
 * The "xstate" datamodel: SCXML expressions are readable source text (guard
 * and action names, function source, JSON literals). When a chart is
 * attached, the host asks the JS resolver once for the function behind each
 * expression of that chart and keeps them in a per-chart table indexed by
 * expression id — the index is an internal detail of the image. Context,
 * events and payloads are napi references owned by the host; they live as
 * long as the System. */
#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors.h>
#include <tinyactors/compile.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

#define SEGMENT_TYPE "tinyactors"

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

/* References are boxed ({ v: value }): before Node-API 10 a reference can
 * only hold objects/functions/symbols, but datamodel values may be primitives. */
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

/* src="..." at compile time (<invoke src>, <data src>, <script src>): the files
 * passed along, by name (file: and ./ stripped), else by base name */
typedef struct nd_files {
    const ta_source *source;
    uint32_t count;
} nd_files;
static ta_status nd_resolve(void *context, ta_string_view base, ta_string_view source,
                            ta_source *out) {
    (void)base;
    const nd_files *files = (const nd_files *)context;
    static const char *const pre[] = {"file://", "file:", "./"};
    for (int k = 0; k < 3; k++) {
        uint32_t l = (uint32_t)strlen(pre[k]);
        if (source.length > l && !memcmp(source.pointer, pre[k], l)) {
            source.pointer += l;
            source.length -= l;
        }
    }
    ta_string_view bn = source;
    for (uint32_t i = 0; i < source.length; i++)
        if (source.pointer[i] == '/')
            bn = (ta_string_view){source.pointer + i + 1, source.length - i - 1};
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < files->count; i++) {
            ta_string_view want = pass ? bn : source;
            if (files->source[i].name.length == want.length &&
                !memcmp(files->source[i].name.pointer, want.pointer, want.length)) {
                *out = files->source[i];
                return TA_OK;
            }
        }
    return TA_ERROR_INVALID;
}

/* {name, xml}[] -> sources (strings owned in `owned`); 0 on a type error (thrown) */
static int read_sources(napi_env environment, napi_value arr, uint32_t n, ta_source *source,
                        char **owned) {
    for (uint32_t i = 0; i < n; i++) {
        napi_value o, name, xml;
        size_t nl = 0, xl = 0;
        napi_get_element(environment, arr, i, &o);
        napi_get_named_property(environment, o, "name", &name);
        napi_get_named_property(environment, o, "xml", &xml);
        owned[2 * i] = get_string(environment, name, &nl);
        owned[2 * i + 1] = get_string(environment, xml, &xl);
        if (!owned[2 * i] || !owned[2 * i + 1]) {
            napi_throw_type_error(environment, NULL, "compile: name and xml must be strings");
            return 0;
        }
        source[i].name = (ta_string_view){owned[2 * i], (uint32_t)nl};
        source[i].text = (ta_string_view){owned[2 * i + 1], (uint32_t)xl};
    }
    return 1;
}

/* compile(sources: {name, xml}[], files?: {name, xml}[]) -> image: charts, plus
 * files their src="..." attributes may name */
static napi_value js_compile(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], result = NULL;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    bool is_array = false, files_array = false;
    if (argc < 1 || napi_is_array(environment, argv[0], &is_array) != napi_ok || !is_array) {
        napi_throw_type_error(environment, NULL,
                              "compile(sources: {name, xml}[], files?: {name, xml}[])");
        return NULL;
    }
    uint32_t n = 0, nf = 0;
    napi_get_array_length(environment, argv[0], &n);
    if (argc > 1 && napi_is_array(environment, argv[1], &files_array) == napi_ok && files_array)
        napi_get_array_length(environment, argv[1], &nf);
    ta_source *source = (ta_source *)calloc(n + nf + 1, sizeof *source);
    char **owned = (char **)calloc(2 * (n + nf + 1), sizeof(char *));
    if (!read_sources(environment, argv[0], n, source, owned) ||
        (nf && !read_sources(environment, argv[1], nf, source + n, owned + 2 * n)))
        goto out;
    {
        nd_files files = {source + n, nf};
        ta_compile_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.resolve = nd_resolve;
        options.resolve_context = &files;
        ta_compile_result *compiled = NULL;
        ta_status status = ta_compile_charts(&options, source, n, &compiled);
        if (status != TA_OK) {
            char message[4096] = "SCXML compilation failed:";
            const ta_diagnostic *diagnostic;
            uint32_t nd = ta_compile_diagnostics(compiled, &diagnostic);
            for (uint32_t i = 0; i < nd && strlen(message) < sizeof message - 200; i++)
                snprintf(message + strlen(message), sizeof message - strlen(message),
                         "\n  %.*s:%u:%u: %.*s", (int)diagnostic[i].file.length,
                         diagnostic[i].file.pointer ? diagnostic[i].file.pointer : "",
                         diagnostic[i].line, diagnostic[i].column,
                         (int)diagnostic[i].message.length, diagnostic[i].message.pointer);
            napi_throw_error(environment, NULL, message);
        } else {
            ta_string_view bytes = ta_compile_image(compiled);
            nd_image *image = (nd_image *)calloc(1, sizeof *image);
            image->bytes = malloc(bytes.length + 8);
            memcpy(image->bytes, bytes.pointer, bytes.length);
            image->length = bytes.length;
            if (ta_image_open(image->bytes, image->length, &image->image) != TA_OK) {
                image_finalize(environment, image, NULL);
                napi_throw_error(environment, NULL, "invalid image");
            } else {
                napi_create_external(environment, image, image_finalize, NULL, &result);
            }
        }
        ta_compile_free(compiled);
    }
out:
    for (uint32_t i = 0; i < 2 * (n + nf); i++)
        free(owned[i]);
    free(owned);
    free(source);
    return result;
}

static nd_image *get_image(napi_env environment, napi_value v) {
    void *p = NULL;
    if (napi_get_value_external(environment, v, &p) != napi_ok || !p) {
        napi_throw_type_error(environment, NULL, "expected an image");
        return NULL;
    }
    return (nd_image *)p;
}

static napi_value js_chart_index(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], r;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_image *image = argc >= 2 ? get_image(environment, argv[0]) : NULL;
    if (!image) return NULL;
    size_t length;
    char *name = get_string(environment, argv[1], &length);
    ta_image_info image_info;
    ta_image_get_info(&image->image, &image_info);
    uint32_t c = TA_NIL;
    for (uint32_t i = 0; name && i < image_info.chart_count && c == TA_NIL; i++) {
        ta_string_view source = ta_image_chart_source(&image->image, i);
        if (source.length == length && !memcmp(source.pointer, name, length)) c = i;
    }
    if (name && c == TA_NIL)
        c = ta_image_chart_by_name(&image->image, (ta_string_view){name, (uint32_t)length});
    free(name);
    napi_create_int32(environment, c == TA_NIL ? -1 : (int32_t)c, &r);
    return r;
}

/* states(image, chart) -> [{ id, parent, kind }]: the state tree (state 0 is the root) */
static napi_value js_states(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], arr;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_image *image = argc >= 2 ? get_image(environment, argv[0]) : NULL;
    if (!image) return NULL;
    uint32_t chart = 0;
    napi_get_value_uint32(environment, argv[1], &chart);
    uint32_t n = ta_image_state_count(&image->image, chart);
    napi_create_array_with_length(environment, n, &arr);
    for (uint32_t state = 0; state < n; state++) {
        ta_state_info state_info;
        if (ta_image_state_info(&image->image, chart, state, &state_info) != TA_OK) break;
        napi_value o, v;
        napi_create_object(environment, &o);
        napi_set_named_property(environment, o, "id", str_value(environment, state_info.id));
        napi_create_int32(environment,
                          state_info.parent == TA_NIL ? -1 : (int32_t)state_info.parent, &v);
        napi_set_named_property(environment, o, "parent", v);
        napi_create_uint32(environment, state_info.kind, &v);
        napi_set_named_property(environment, o, "kind", v);
        napi_set_element(environment, arr, state, o);
    }
    return arr;
}

/* ---- the datamodel host ---------------------------------------------------------------- */

typedef struct nd_chart {
    const ta_image *image;
    uint32_t chart, count;
    napi_ref *function;  /* per expression id: resolved function (or NULL) */
    napi_ref ecmascript; /* "ecmascript" charts: the chart of bindings/shared/ecmascript.mjs */
} nd_chart;

typedef struct nd_instance {
    uint64_t serial, datamodel_handle;
    nd_chart *chart;
    napi_ref context, event, init, in_function, done_config;
    napi_ref object; /* "ecmascript" sessions: the session of bindings/shared/ecmascript.mjs */
    ta_datamodel_environment *environment;
    char *string;
    int live;
    uint32_t next_free;
} nd_instance;

typedef struct nd_grave { /* a finished session: its last context (done roots only) */
    uint64_t serial;
    napi_ref context, done_config;
} nd_grave;

typedef struct nd_error {
    uint64_t serial;
    napi_ref error;
} nd_error;

/* One JS System: the datamodel host (values behind napi references, owned by
 * it) and the ta_host (segments, routing, versions, migration) it serves. */
typedef struct nd_host {
    napi_env environment; /* environment of the current native call */
    napi_ref resolver;
    napi_ref ecmascript; /* the ECMAScript datamodel (bindings/shared/ecmascript.mjs) */
    napi_ref codec; /* { encode(value) -> Uint8Array, decode(Uint8Array) -> value } (hibernation) */
    nd_instance *evaluating; /* instance whose expression is being evaluated */
    nd_instance *instances;  /* handle = index + 1 */
    uint32_t instance_count, instance_capacity, free_instance;
    napi_ref *payloads; /* payload = index + 1 */
    uint32_t *refs;
    uint32_t payload_count, payload_capacity, free_payload;
    uint32_t live_payloads;
    int hibernating; /* destroy() during ta_host_hibernate: not the session's end */
    int destroyed;   /* destroy() during ta_host_free */
    ta_host *host;
    char base[TA_ADDRESS_SYSTEM_MAX + 16]; /* tinyactors://<id>/ */
    napi_ref *images; /* per segment: the image (kept alive until the segment is freed) */
    uint32_t image_count, image_capacity;
    uint64_t *keys; /* serial -> instance handle (open addressing, 0 = empty) */
    uint32_t *values, map_capacity, map_count;
    uint32_t session_count; /* live datamodel instances */
    nd_grave *graves;
    uint32_t grave_count, grave_capacity;
    nd_error *errors;
    uint32_t error_count, error_capacity;
    nd_chart **charts; /* attached charts (restore finds an instance's chart here) */
    uint32_t chart_count, chart_capacity;
    napi_ref store_error;      /* why the last serialize failed (the exception), or NULL */
    ta_payload cached_payload; /* the payload whose encoding is in cached_encoding (0: none) */
    unsigned char *cached_encoding;
    size_t cached_length, cached_capacity;
} nd_host;

static void chart_add(nd_host *host, nd_chart *chart) {
    if (host->chart_count == host->chart_capacity) {
        uint32_t nc = host->chart_capacity ? host->chart_capacity * 2 : 16;
        nd_chart **l = (nd_chart **)realloc(host->charts, sizeof(nd_chart *) * nc);
        if (!l) return;
        host->charts = l;
        host->chart_capacity = nc;
    }
    host->charts[host->chart_count++] = chart;
}

/* -- serial -> instance map (host-wide; serials are never reused) */
static void map_put(nd_host *s, uint64_t k, uint32_t v);
static void map_grow(nd_host *s) {
    uint64_t *ok = s->keys;
    uint32_t *ov = s->values, oc = s->map_capacity;
    s->map_capacity = oc ? oc * 2 : 64;
    s->keys = (uint64_t *)calloc(s->map_capacity, sizeof(uint64_t));
    s->values = (uint32_t *)calloc(s->map_capacity, sizeof(uint32_t));
    s->map_count = 0;
    for (uint32_t i = 0; i < oc; i++)
        if (ok[i] && ov[i]) map_put(s, ok[i], ov[i]);
    free(ok);
    free(ov);
}
static void map_put(nd_host *s, uint64_t k, uint32_t v) {
    if ((s->map_count + 1) * 2 > s->map_capacity) map_grow(s);
    uint32_t i = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 40) & (s->map_capacity - 1);
    while (s->keys[i] && s->keys[i] != k)
        i = (i + 1) & (s->map_capacity - 1);
    if (!s->keys[i]) s->map_count++;
    s->keys[i] = k;
    s->values[i] = v; /* v == 0: removed (tombstone keeps probing intact) */
}
static uint32_t map_get(nd_host *s, uint64_t k) {
    if (!s->map_capacity) return 0;
    uint32_t i = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 40) & (s->map_capacity - 1);
    while (s->keys[i]) {
        if (s->keys[i] == k) return s->values[i];
        i = (i + 1) & (s->map_capacity - 1);
    }
    return 0;
}

/* -- payloads (refcounted napi references, free-listed) */
static ta_payload payload_new(nd_host *host, napi_value v) {
    uint32_t i;
    if (host->free_payload) {
        i = host->free_payload - 1;
        host->free_payload = (uint32_t)(uintptr_t)host->payloads[i];
    } else {
        if (host->payload_count == host->payload_capacity) {
            uint32_t nc = host->payload_capacity ? host->payload_capacity * 2 : 64;
            host->payloads = (napi_ref *)realloc(host->payloads, sizeof(napi_ref) * nc);
            host->refs = (uint32_t *)realloc(host->refs, sizeof(uint32_t) * nc);
            host->payload_capacity = nc;
        }
        i = host->payload_count++;
    }
    host->payloads[i] = make_ref(host->environment, v);
    host->refs[i] = 1;
    host->live_payloads++;
    return (ta_payload)i + 1;
}

static napi_value payload_get(nd_host *host, ta_payload payload) {
    if (!payload || payload > host->payload_count || !host->refs[payload - 1])
        return undef(host->environment);
    return get_ref(host->environment, host->payloads[payload - 1]);
}

static void nd_retain(void *d, ta_payload payload) {
    nd_host *host = (nd_host *)d;
    if (payload && payload <= host->payload_count && host->refs[payload - 1])
        host->refs[payload - 1]++;
}

static void nd_release(void *d, ta_payload payload) {
    nd_host *host = (nd_host *)d;
    if (!payload || payload > host->payload_count || !host->refs[payload - 1]) return;
    if (--host->refs[payload - 1] == 0) {
        if (host->cached_payload == payload) host->cached_payload = 0;
        napi_delete_reference(host->environment, host->payloads[payload - 1]);
        host->payloads[payload - 1] = (napi_ref)(uintptr_t)host->free_payload;
        host->free_payload = (uint32_t)payload;
        host->live_payloads--;
    }
}

static nd_instance *I(nd_host *host, uint64_t handle) {
    return handle && handle <= host->instance_count && host->instances[handle - 1].live
               ? &host->instances[handle - 1]
               : NULL;
}

/* ES instance and <foreach> handles are tagged in their high word: the
 * slot in the unified table is the rest. */
#define ECMASCRIPT_HANDLE (UINT64_C(1) << 32)
static nd_instance *E_I(nd_host *host, uint64_t handle) {
    return I(host, handle >= ECMASCRIPT_HANDLE ? handle - ECMASCRIPT_HANDLE : handle);
}

/* Record a pending JS exception against the instance (if any) and clear it. */
static void take_exception(nd_host *host, nd_instance *instance) {
    bool pending = false;
    napi_is_exception_pending(host->environment, &pending);
    if (!pending) return;
    napi_value exc;
    napi_get_and_clear_last_exception(host->environment, &exc);
    nd_host *s = host;
    if (s->error_count == s->error_capacity) {
        s->error_capacity = s->error_capacity ? s->error_capacity * 2 : 8;
        s->errors = (nd_error *)realloc(s->errors, sizeof(nd_error) * s->error_capacity);
    }
    s->errors[s->error_count].serial = instance ? instance->serial : 0;
    s->errors[s->error_count].error = make_ref(host->environment, exc);
    s->error_count++;
}

static void throw_unknown(nd_host *host, nd_instance *instance, ta_expression expression) {
    ta_expression_info info;
    char message[600];
    if (ta_image_expression(instance->chart->image, instance->chart->chart, expression, &info) ==
        TA_OK)
        snprintf(message, sizeof message, "no implementation for expression `%.*s`",
                 (int)(info.source.length > 500 ? 500 : info.source.length), info.source.pointer);
    else
        snprintf(message, sizeof message, "unknown expression");
    napi_throw_error(host->environment, NULL, message);
    take_exception(host, instance);
}

/* $.in(stateID) for guards */
static napi_value js_in(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], r;
    void *data;
    napi_get_cb_info(environment, info, &argc, argv, NULL, &data);
    nd_host *host = (nd_host *)data;
    bool yes = false;
    size_t length;
    char *id = argc ? get_string(environment, argv[0], &length) : NULL;
    nd_instance *instance = host->evaluating;
    if (id && instance && instance->environment) {
        uint32_t state = ta_image_state_by_name(instance->chart->image, instance->chart->chart,
                                                (ta_string_view){id, (uint32_t)length});
        yes = state != TA_NIL && ta_datamodel_environment_in(instance->environment, state);
    }
    free(id);
    napi_get_boolean(environment, yes, &r);
    return r;
}

/* Build the argument object { context, event, input } for user functions. */
static napi_value make_args(nd_host *host, nd_instance *instance) {
    napi_value o, init;
    napi_create_object(host->environment, &o);
    napi_set_named_property(host->environment, o, "context",
                            get_ref(host->environment, instance->context));
    napi_set_named_property(host->environment, o, "event",
                            get_ref(host->environment, instance->event));
    init = get_ref(host->environment, instance->init);
    napi_valuetype t;
    napi_typeof(host->environment, init, &t);
    if (t == napi_object) {
        napi_value input;
        napi_get_named_property(host->environment, init, "input", &input);
        napi_set_named_property(host->environment, o, "input", input);
    }
    return o;
}

/* Evaluate an expression: call its resolved function with (args, $). */
static napi_value eval_value(nd_host *host, nd_instance *instance, ta_expression expression) {
    nd_chart *chart = instance->chart;
    if (expression >= chart->count || !chart->function[expression]) {
        throw_unknown(host, instance, expression);
        return NULL;
    }
    napi_value function = get_ref(host->environment, chart->function[expression]), global,
               result = NULL, argv[2];
    napi_get_global(host->environment, &global);
    argv[0] = make_args(host, instance);
    napi_create_object(host->environment, &argv[1]);
    napi_set_named_property(host->environment, argv[1], "in",
                            get_ref(host->environment, instance->in_function));
    char self[192];
    snprintf(self, sizeof self, "%s#_scxml_%llu", host->base, (unsigned long long)instance->serial);
    napi_set_named_property(
        host->environment, argv[1], "self",
        str_value(host->environment, (ta_string_view){self, (uint32_t)strlen(self)}));
    nd_instance *outer = host->evaluating;
    host->evaluating = instance;
    napi_status cs = napi_call_function(host->environment, global, function, 2, argv, &result);
    host->evaluating = outer;
    if (cs != napi_ok) {
        take_exception(host, instance);
        return NULL;
    }
    return result;
}

static ta_status nd_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    nd_host *host = (nd_host *)d;
    nd_chart *binding_chart = (nd_chart *)calloc(1, sizeof *binding_chart);
    if (!binding_chart) return TA_ERROR_NO_MEMORY;
    binding_chart->image = chart->image;
    binding_chart->chart = chart->chart;
    binding_chart->count = ta_image_expression_count(chart->image, chart->chart);
    binding_chart->function =
        (napi_ref *)calloc(binding_chart->count ? binding_chart->count : 1, sizeof(napi_ref));
    /* resolver(chartName, texts) -> functions, aligned with expression ids */
    napi_value texts, argv[2], global, result;
    napi_create_array_with_length(host->environment, binding_chart->count, &texts);
    for (uint32_t e = 0; e < binding_chart->count; e++) {
        ta_expression_info info;
        if (ta_image_expression(binding_chart->image, binding_chart->chart, e, &info) == TA_OK)
            napi_set_element(host->environment, texts, e,
                             str_value(host->environment, info.source));
    }
    argv[0] = str_value(host->environment,
                        ta_image_chart_source(binding_chart->image, binding_chart->chart));
    argv[1] = texts;
    napi_get_global(host->environment, &global);
    if (napi_call_function(host->environment, global, get_ref(host->environment, host->resolver), 2,
                           argv, &result) != napi_ok) {
        take_exception(host, NULL);
        *context = binding_chart;
        chart_add(host, binding_chart);
        return TA_OK; /* unresolved expressions fail when evaluated */
    }
    for (uint32_t e = 0; e < binding_chart->count; e++) {
        napi_value f;
        napi_valuetype t = napi_undefined;
        if (napi_get_element(host->environment, result, e, &f) == napi_ok)
            napi_typeof(host->environment, f, &t);
        if (t == napi_function) binding_chart->function[e] = make_ref(host->environment, f);
    }
    *context = binding_chart;
    chart_add(host, binding_chart);
    return TA_OK;
}

static void nd_detach(void *d, void *context) { /* a segment is freed (or the host) */
    nd_host *host = (nd_host *)d;
    nd_chart *chart = (nd_chart *)context;
    if (!chart) return;
    for (uint32_t i = 0; i < host->chart_count; i++)
        if (host->charts[i] == chart) host->charts[i] = host->charts[--host->chart_count];
    for (uint32_t e = 0; e < chart->count; e++)
        if (chart->function[e]) napi_delete_reference(host->environment, chart->function[e]);
    if (chart->ecmascript) napi_delete_reference(host->environment, chart->ecmascript);
    free(chart->function);
    free(chart);
}

static nd_instance *instance_new(nd_host *h, void *chart_context,
                                 const ta_datamodel_system_variables *sv, uint64_t *out);

static ta_status nd_create(void *d, void *chart_context,
                           const ta_datamodel_system_variables *variables, ta_payload init,
                           uint64_t *out) {
    nd_host *host = (nd_host *)d;
    nd_instance *instance = instance_new(host, chart_context, variables, out);
    if (instance) { /* xstate: the context is an object, one property per <data> */
        napi_value context;
        napi_create_object(host->environment, &context);
        set_ref(host->environment, &instance->context, context);
    }
    if (init && instance) set_ref(host->environment, &instance->init, payload_get(host, init));
    nd_release(host, init); /* ownership of the init payload passes to us */
    return instance ? TA_OK : TA_ERROR_NO_MEMORY;
}

static nd_instance *instance_new(nd_host *host, void *chart_context,
                                 const ta_datamodel_system_variables *variables, uint64_t *out) {
    uint32_t i;
    if (host->free_instance) {
        i = host->free_instance - 1;
        host->free_instance = host->instances[i].next_free;
    } else {
        if (host->instance_count == host->instance_capacity) {
            host->instance_capacity = host->instance_capacity ? host->instance_capacity * 2 : 16;
            host->instances = (nd_instance *)realloc(host->instances,
                                                     sizeof(nd_instance) * host->instance_capacity);
            if (!host->instances) return NULL;
        }
        i = host->instance_count++;
    }
    nd_instance *instance = &host->instances[i];
    memset(instance, 0, sizeof *instance);
    instance->datamodel_handle = i + 1;
    instance->chart = (nd_chart *)chart_context;
    instance->live = 1;
    host->session_count++;
    for (uint32_t k = 0; k < variables->sessionid.length; k++)
        if (variables->sessionid.pointer[k] >= '0' && variables->sessionid.pointer[k] <= '9')
            instance->serial =
                instance->serial * 10 + (uint64_t)(variables->sessionid.pointer[k] - '0');
    napi_value f;
    napi_create_function(host->environment, "In", NAPI_AUTO_LENGTH, js_in, host, &f);
    set_ref(host->environment, &instance->in_function, f);
    map_put(host, instance->serial, i + 1);
    *out = i + 1;
    return instance;
}

/* ---- hibernation: the instance's values through the JS codec (shared with the
 * web binding, so blobs mean the same everywhere) */
static napi_value codec_function(nd_host *host, const char *name) {
    napi_value c = get_ref(host->environment, host->codec), f = NULL;
    napi_valuetype t = napi_undefined;
    napi_typeof(host->environment, c, &t);
    if (t != napi_object) return NULL;
    napi_get_named_property(host->environment, c, name, &f);
    napi_typeof(host->environment, f, &t);
    return t == napi_function ? f : NULL;
}

/* A serialize that failed: keep the exception (it says what cannot be stored) for
 * storeError(), instead of reporting it as an error of the session. */
static void keep_error(nd_host *host) {
    bool pending = false;
    napi_is_exception_pending(host->environment, &pending);
    if (!pending) return;
    napi_value exc;
    napi_get_and_clear_last_exception(host->environment, &exc);
    set_ref(host->environment, &host->store_error, exc);
}

static napi_value codec_call(nd_host *host, const char *name, napi_value arg) {
    napi_value f = codec_function(host, name), global, r;
    if (!f) return NULL;
    napi_get_global(host->environment, &global);
    if (napi_call_function(host->environment, global, f, 1, &arg, &r) != napi_ok) {
        keep_error(host);
        return NULL;
    }
    return r;
}

static napi_value bytes_value(napi_env environment, const void *p, size_t n);
static int bytes_of(napi_env environment, napi_value v, void **data, size_t *n);

/* Payloads of delayed sends in export blobs: values through the codec (both
 * datamodels share the payload table), so another process can adopt the blob.
 * The export sizes a payload, then writes it: the encoding of the last one is
 * kept for that. */
static ta_status nd_payload_serialize(void *d, ta_payload payload, void *buffer, size_t capacity,
                                      size_t *length) {
    nd_host *host = (nd_host *)d;
    *length = 0;
    if (!buffer || payload != host->cached_payload) {
        host->cached_payload = 0;
        napi_value bytes = codec_call(host, "encode", payload_get(host, payload));
        void *data = NULL;
        size_t n = 0;
        if (!bytes || !bytes_of(host->environment, bytes, &data, &n)) return TA_ERROR_UNSUPPORTED;
        if (n > host->cached_capacity) {
            unsigned char *nb = (unsigned char *)realloc(host->cached_encoding, n);
            if (!nb) return TA_ERROR_NO_MEMORY;
            host->cached_encoding = nb;
            host->cached_capacity = n;
        }
        if (n) memcpy(host->cached_encoding, data, n);
        host->cached_length = n;
        host->cached_payload = payload;
    }
    *length = host->cached_length;
    if (host->cached_length > capacity) return TA_ERROR_NO_MEMORY;
    if (host->cached_length) memcpy(buffer, host->cached_encoding, host->cached_length);
    return TA_OK;
}

static ta_status nd_payload_deserialize(void *d, const void *buffer, size_t length,
                                        ta_payload *out) {
    nd_host *host = (nd_host *)d;
    napi_value v = codec_call(host, "decode", bytes_value(host->environment, buffer, length));
    *out = v ? payload_new(host, v) : 0;
    return v ? TA_OK : TA_ERROR_INVALID;
}

static ta_status nd_serialize(void *d, uint64_t handle, void *buffer, size_t capacity,
                              size_t *length) {
    nd_host *host = (nd_host *)d;
    nd_instance *instance = I(host, handle);
    napi_value enc = instance ? codec_function(host, "encode") : NULL, o, global, bytes;
    *length = 0;
    if (!enc) return TA_ERROR_UNSUPPORTED;
    napi_create_object(host->environment, &o);
    napi_set_named_property(host->environment, o, "context",
                            get_ref(host->environment, instance->context));
    napi_set_named_property(host->environment, o, "event",
                            get_ref(host->environment, instance->event));
    napi_set_named_property(host->environment, o, "init",
                            get_ref(host->environment, instance->init));
    if (instance->done_config)
        napi_set_named_property(host->environment, o, "doneConfig",
                                get_ref(host->environment, instance->done_config));
    napi_get_global(host->environment, &global);
    if (napi_call_function(host->environment, global, enc, 1, &o, &bytes) != napi_ok) {
        keep_error(host); /* e.g. functions in the context: stays resident */
        return TA_ERROR_UNSUPPORTED;
    }
    napi_typedarray_type tt;
    size_t n = 0, offset = 0;
    void *data = NULL;
    napi_value ab;
    if (napi_get_typedarray_info(host->environment, bytes, &tt, &n, &data, &ab, &offset) !=
            napi_ok ||
        tt != napi_uint8_array)
        return TA_ERROR_UNSUPPORTED;
    *length = n;
    if (n > capacity) return TA_ERROR_NO_MEMORY;
    if (n) memcpy(buffer, data, n);
    return TA_OK;
}

static ta_status nd_deserialize(void *d, void *chart_context,
                                const ta_datamodel_system_variables *variables, const void *buffer,
                                size_t length, uint64_t *out) {
    nd_host *host = (nd_host *)d;
    napi_value dec = codec_function(host, "decode"), ab, u8, global, o;
    if (!dec) return TA_ERROR_UNSUPPORTED;
    void *data = NULL;
    napi_create_arraybuffer(host->environment, length, &data, &ab);
    if (length) memcpy(data, buffer, length);
    napi_create_typedarray(host->environment, napi_uint8_array, length, ab, 0, &u8);
    napi_get_global(host->environment, &global);
    if (napi_call_function(host->environment, global, dec, 1, &u8, &o) != napi_ok) {
        take_exception(host, NULL);
        return TA_ERROR_INVALID;
    }
    nd_instance *instance = instance_new(host, chart_context, variables, out);
    if (!instance) return TA_ERROR_NO_MEMORY;
    napi_value v;
    napi_get_named_property(host->environment, o, "context", &v);
    set_ref(host->environment, &instance->context, v);
    napi_get_named_property(host->environment, o, "event", &v);
    set_ref(host->environment, &instance->event, v);
    napi_get_named_property(host->environment, o, "init", &v);
    set_ref(host->environment, &instance->init, v);
    bool has = false;
    napi_has_named_property(host->environment, o, "doneConfig", &has);
    if (has) {
        napi_get_named_property(host->environment, o, "doneConfig", &v);
        set_ref(host->environment, &instance->done_config, v);
    }
    return TA_OK;
}

static void instance_free(nd_host *host, uint32_t handle) {
    nd_instance *instance = &host->instances[handle - 1];
    if (!instance->live) return;
    set_ref(host->environment, &instance->context, NULL);
    set_ref(host->environment, &instance->event, NULL);
    set_ref(host->environment, &instance->init, NULL);
    set_ref(host->environment, &instance->in_function, NULL);
    set_ref(host->environment, &instance->done_config, NULL);
    set_ref(host->environment, &instance->object, NULL);
    free(instance->string);
    instance->string = NULL;
    instance->live = 0;
    if (host->session_count) host->session_count--;
    instance->next_free = host->free_instance;
    host->free_instance = handle;
}

static void nd_destroy(void *d, uint64_t handle) {
    nd_host *host = (nd_host *)d;
    nd_instance *instance = I(host, handle);
    if (!instance) return;
    nd_host *s = host;
    if (!host->destroyed && !host->hibernating) { /* the last context, until run() reports the end
                                              (migrations destroy too: those are dropped) */
        if (s->grave_count == s->grave_capacity) {
            s->grave_capacity = s->grave_capacity ? s->grave_capacity * 2 : 8;
            s->graves = (nd_grave *)realloc(s->graves, sizeof(nd_grave) * s->grave_capacity);
        }
        nd_grave *grave = &s->graves[s->grave_count++];
        grave->serial = instance->serial;
        grave->context = make_ref(host->environment, get_ref(host->environment, instance->context));
        grave->done_config = instance->done_config;
        instance->done_config = NULL;
    }
    map_put(s, instance->serial, 0);
    instance_free(host, (uint32_t)handle);
}

#define ENTER()                                                                                    \
    nd_host *host = (nd_host *)d;                                                                  \
    nd_instance *instance = I(host, handle);                                                       \
    if (!instance) return TA_ERROR_EXECUTION;                                                      \
    instance->environment = environment

static ta_status nd_init_data(void *d, uint64_t handle, ta_datamodel_environment *environment,
                              uint32_t declaration) {
    ENTER();
    ta_data_info data_info;
    ta_status status = TA_OK;
    if (ta_image_data(instance->chart->image, instance->chart->chart, declaration, &data_info) !=
        TA_OK)
        status = TA_ERROR_EXECUTION;
    else if (data_info.id.length == 7 && !memcmp(data_info.id.pointer, "context", 7)) {
        /* an opaque context factory: the whole context at once */
        napi_value v = data_info.expression != TA_NIL
                           ? eval_value(host, instance, data_info.expression)
                           : undef(host->environment);
        if (v)
            set_ref(host->environment, &instance->context, v);
        else
            status = TA_ERROR_EXECUTION;
    } else { /* <data id="prop">: one context property */
        napi_value v = data_info.expression != TA_NIL
                           ? eval_value(host, instance, data_info.expression)
                           : undef(host->environment);
        if (v)
            napi_set_property(host->environment, get_ref(host->environment, instance->context),
                              str_value(host->environment, data_info.id), v);
        else
            status = TA_ERROR_EXECUTION;
    }
    instance->environment = NULL;
    return status;
}

/* SCXML platform event names -> xstate event types */
static napi_value event_type(napi_env environment, ta_string_view name) {
    napi_value t;
    char buffer[512];
    if (name.length > 12 && !memcmp(name.pointer, "done.invoke.", 12)) {
        snprintf(buffer, sizeof buffer, "xstate.done.actor.%.*s", (int)(name.length - 12),
                 name.pointer + 12);
        napi_create_string_utf8(environment, buffer, NAPI_AUTO_LENGTH, &t);
    } else if (name.length > 11 && !memcmp(name.pointer, "done.state.", 11)) {
        snprintf(buffer, sizeof buffer, "xstate.done.state.%.*s", (int)(name.length - 11),
                 name.pointer + 11);
        napi_create_string_utf8(environment, buffer, NAPI_AUTO_LENGTH, &t);
    } else {
        napi_create_string_utf8(environment, name.pointer, name.length, &t);
    }
    return t;
}

/* active state ids of the instance, as a JS array */
static napi_value config_array(nd_host *host, nd_instance *instance,
                               ta_datamodel_environment *environment) {
    napi_value arr;
    napi_create_array(host->environment, &arr);
    uint32_t n = ta_image_state_count(instance->chart->image, instance->chart->chart), k = 0;
    for (uint32_t state = 1; state < n; state++)
        if (ta_datamodel_environment_in(environment, state))
            napi_set_element(
                host->environment, arr, k++,
                str_value(host->environment, ta_image_state_name(instance->chart->image,
                                                                 instance->chart->chart, state)));
    return arr;
}

static ta_status nd_bind_event(void *d, uint64_t handle, ta_datamodel_environment *environment,
                               const ta_event_view *view) {
    nd_host *host = (nd_host *)d;
    nd_instance *instance = I(host, handle);
    if (!instance) return TA_ERROR_EXECUTION;
    napi_value data = payload_get(host, view->data), obj;
    napi_valuetype t;
    napi_typeof(host->environment, data, &t);
    int done_state = view->name.length > 11 && !memcmp(view->name.pointer, "done.state.", 11);
    int platform = view->type == TA_EVENT_TYPE_PLATFORM || done_state ||
                   (view->name.length > 12 && !memcmp(view->name.pointer, "done.invoke.", 12));
    int is_event = 0; /* an xstate event object: its type is the event's name */
    if (t == napi_object && !platform) {
        napi_value ty;
        bool equal = false;
        if (napi_get_named_property(host->environment, data, "type", &ty) == napi_ok &&
            napi_strict_equals(host->environment, ty, str_value(host->environment, view->name),
                               &equal) == napi_ok)
            is_event = equal;
    }
    if (is_event) {
        obj = data; /* delivered as-is; other data (an SCXML document's <param>s) as event.data */
    } else {
        napi_create_object(host->environment, &obj);
        napi_set_named_property(host->environment, obj, "type",
                                event_type(host->environment, view->name));
        int error = view->name.length > 6 && !memcmp(view->name.pointer, "error.", 6);
        if (t != napi_undefined)
            napi_set_named_property(host->environment, obj, platform && !error ? "output" : "data",
                                    data);
    }
    set_ref(host->environment, &instance->event, obj);
    /* a done.state.* event: remember which final configuration produced it
     * (a parallel machine's value once it is done) */
    if (done_state)
        set_ref(host->environment, &instance->done_config,
                config_array(host, instance, environment));
    return TA_OK;
}

static ta_status nd_condition(void *d, uint64_t handle, ta_datamodel_environment *environment,
                              ta_expression expression, int *out) {
    ENTER();
    napi_value v = eval_value(host, instance, expression), b;
    instance->environment = NULL;
    if (!v) return TA_ERROR_EXECUTION;
    bool r = false;
    napi_coerce_to_bool(host->environment, v, &b);
    napi_get_value_bool(host->environment, b, &r);
    *out = r;
    return TA_OK;
}

static ta_status to_str(nd_host *host, nd_instance *instance, napi_value v, ta_string_view *out) {
    napi_value s;
    if (napi_coerce_to_string(host->environment, v, &s) != napi_ok) {
        take_exception(host, instance);
        return TA_ERROR_EXECUTION;
    }
    size_t length;
    free(instance->string);
    instance->string = get_string(host->environment, s, &length);
    if (!instance->string) return TA_ERROR_EXECUTION;
    out->pointer = instance->string;
    out->length = (uint32_t)length;
    return TA_OK;
}

/* String-valued attributes (eventexpr, targetexpr, delayexpr): event objects
 * stand for their type, numbers for milliseconds. */
static ta_status nd_string(void *d, uint64_t handle, ta_datamodel_environment *environment,
                           ta_expression expression, ta_string_view *out) {
    ENTER();
    napi_value v = eval_value(host, instance, expression);
    ta_status status = TA_ERROR_EXECUTION;
    if (v) {
        napi_valuetype t;
        napi_typeof(host->environment, v, &t);
        if (t == napi_object) {
            napi_value ty;
            napi_get_named_property(host->environment, v, "type", &ty);
            v = ty;
        } else if (t == napi_number) {
            double ms = 0;
            napi_get_value_double(host->environment, v, &ms);
            char buffer[64];
            snprintf(buffer, sizeof buffer, "%.3fms", ms < 0 ? 0 : ms);
            napi_create_string_utf8(host->environment, buffer, NAPI_AUTO_LENGTH, &v);
        }
        status = to_str(host, instance, v, out);
    }
    instance->environment = NULL;
    return status;
}

static napi_value object_assign(nd_host *host, nd_instance *instance, napi_value a, napi_value b) {
    napi_value global, object, assign, fresh, argv[3], merged;
    napi_get_global(host->environment, &global);
    napi_get_named_property(host->environment, global, "Object", &object);
    napi_get_named_property(host->environment, object, "assign", &assign);
    napi_create_object(host->environment, &fresh);
    argv[0] = fresh;
    argv[1] = a;
    argv[2] = b;
    if (napi_call_function(host->environment, object, assign, 3, argv, &merged) != napi_ok) {
        take_exception(host, instance);
        return NULL;
    }
    return merged;
}

/* <assign location="key" expr="..."/> sets one context property;
 * location="context" (an opaque assign() function) merges a partial context.
 * xstate's assign semantics: a new context object each time. */
static ta_status nd_assign(void *d, uint64_t handle, ta_datamodel_environment *environment,
                           ta_expression location, ta_expression value) {
    ENTER();
    ta_expression_info li;
    ta_status status = TA_ERROR_EXECUTION;
    napi_value v = eval_value(host, instance, value);
    if (v && ta_image_expression(instance->chart->image, instance->chart->chart, location, &li) ==
                 TA_OK) {
        napi_value partial = v, context_value = get_ref(host->environment, instance->context),
                   merged;
        if (!(li.source.length == 7 && !memcmp(li.source.pointer, "context", 7))) {
            napi_create_object(host->environment, &partial);
            napi_set_property(host->environment, partial, str_value(host->environment, li.source),
                              v);
        }
        napi_valuetype t;
        napi_typeof(host->environment, partial, &t);
        if (t == napi_object && (merged = object_assign(host, instance, context_value, partial))) {
            set_ref(host->environment, &instance->context, merged);
            status = TA_OK;
        }
    }
    instance->environment = NULL;
    return status;
}

static ta_status nd_script(void *d, uint64_t handle, ta_datamodel_environment *environment,
                           ta_expression expression) {
    ENTER();
    napi_value v = eval_value(host, instance, expression);
    instance->environment = NULL;
    return v ? TA_OK : TA_ERROR_EXECUTION;
}

static ta_status nd_log(void *d, uint64_t handle, ta_datamodel_environment *environment,
                        ta_expression expression, ta_string_view *out) {
    ENTER();
    napi_value v = eval_value(host, instance, expression);
    ta_status status = TA_ERROR_EXECUTION;
    if (v) {
        napi_valuetype t;
        napi_typeof(host->environment, v, &t);
        if (t == napi_object) { /* JSON for objects */
            napi_value global, json, stringify, s;
            napi_get_global(host->environment, &global);
            napi_get_named_property(host->environment, global, "JSON", &json);
            napi_get_named_property(host->environment, json, "stringify", &stringify);
            if (napi_call_function(host->environment, json, stringify, 1, &v, &s) == napi_ok)
                v = s;
            else
                take_exception(host, instance);
        }
        status = to_str(host, instance, v, out);
    }
    instance->environment = NULL;
    return status;
}

static ta_status nd_capture(void *d, uint64_t handle, ta_datamodel_environment *environment,
                            const ta_datamodel_capture *capture, ta_payload *out) {
    ENTER();
    napi_value v = NULL;
    if (capture->content != TA_NIL) {
        v = eval_value(host, instance, capture->content);
    } else {
        napi_create_object(host->environment, &v);
        for (uint32_t i = 0; v && i < capture->param_count; i++) {
            napi_value x = eval_value(host, instance, capture->params[2 * i + 1]);
            if (!x) {
                v = NULL;
                break;
            }
            ta_string_view name = ta_image_string(instance->chart->image, capture->params[2 * i]);
            napi_set_property(host->environment, v, str_value(host->environment, name), x);
        }
    }
    instance->environment = NULL;
    if (!v) return TA_ERROR_EXECUTION;
    *out = payload_new(host, v);
    return TA_OK;
}

/* Live migration: the previous bindings are the old context's properties
 * (error.platform.upgrade's event.data); properties the new chart does not
 * declare as <data> are dropped (unless it keeps one opaque context). */
static ta_status nd_migrate(void *d, uint64_t handle, ta_datamodel_environment *environment,
                            ta_payload *bindings) {
    ENTER();
    instance->environment = NULL;
    napi_value old = get_ref(host->environment, instance->context), prev, context_value, keys;
    napi_valuetype t;
    napi_typeof(host->environment, old, &t);
    *bindings = 0;
    if (t != napi_object) return TA_OK;
    napi_create_object(host->environment, &prev);
    napi_create_object(host->environment, &context_value);
    const ta_image *image = instance->chart->image;
    uint32_t chart = instance->chart->chart, nd = ta_image_data_count(image, chart), opaque = 0;
    for (uint32_t i = 0; i < nd; i++) {
        ta_data_info data_info;
        if (ta_image_data(image, chart, i, &data_info) == TA_OK && data_info.id.length == 7 &&
            !memcmp(data_info.id.pointer, "context", 7))
            opaque = 1;
    }
    uint32_t n = 0;
    napi_get_property_names(host->environment, old, &keys);
    napi_get_array_length(host->environment, keys, &n);
    for (uint32_t k = 0; k < n; k++) {
        napi_value key, v;
        napi_get_element(host->environment, keys, k, &key);
        napi_get_property(host->environment, old, key, &v);
        napi_set_property(host->environment, prev, key, v);
        size_t kl = 0;
        char *ks = get_string(host->environment, key, &kl);
        int keep = opaque;
        for (uint32_t i = 0; i < nd && !keep && ks; i++) {
            ta_data_info data_info;
            keep = ta_image_data(image, chart, i, &data_info) == TA_OK &&
                   data_info.id.length == kl && !memcmp(data_info.id.pointer, ks, kl);
        }
        free(ks);
        if (keep) napi_set_property(host->environment, context_value, key, v);
    }
    set_ref(host->environment, &instance->context, context_value);
    *bindings = payload_new(host, prev);
    return TA_OK;
}

static const ta_datamodel_vtable nd_vt = {sizeof(ta_datamodel_vtable),
                                          0,
                                          "xstate",
                                          nd_attach,
                                          nd_detach,
                                          nd_create,
                                          nd_destroy,
                                          nd_init_data,
                                          nd_bind_event,
                                          NULL,
                                          nd_condition,
                                          nd_string,
                                          nd_assign,
                                          NULL,
                                          nd_script,
                                          nd_log,
                                          NULL,
                                          NULL,
                                          NULL,
                                          nd_capture,
                                          nd_retain,
                                          nd_release,
                                          NULL,
                                          nd_serialize,
                                          nd_deserialize,
                                          nd_migrate,
                                          NULL, /* to_wire: links encode JS values in JS */
                                          NULL,
                                          nd_payload_serialize,
                                          nd_payload_deserialize};

/* ---- the ECMAScript datamodel (SCXML documents): forwards to bindings/shared/ecmascript.mjs,
 * which works on plain values; payloads stay in the table above (shared with "xstate") */

/* es.<method>(argv...) with `in` as the evaluating session (In()); NULL on an exception */
static napi_value ecmascript_call(nd_host *host, nd_instance *instance, const char *method,
                                  size_t argc, napi_value *argv) {
    napi_value ecmascript = get_ref(host->environment, host->ecmascript), fn, r = NULL;
    if (napi_get_named_property(host->environment, ecmascript, method, &fn) != napi_ok) return NULL;
    nd_instance *outer = host->evaluating;
    host->evaluating = instance;
    napi_status status = napi_call_function(host->environment, ecmascript, fn, argc, argv, &r);
    host->evaluating = outer;
    if (status != napi_ok) {
        take_exception(host, instance);
        return NULL;
    }
    return r;
}

static int truthy(nd_host *host, napi_value v) {
    bool b = false;
    return v && napi_get_value_bool(host->environment, v, &b) == napi_ok && b;
}

static napi_value u32_value(napi_env environment, uint32_t x) {
    napi_value v;
    napi_create_uint32(environment, x, &v);
    return v;
}

static napi_value expression_argument(napi_env environment, uint32_t e) { /* TA_NIL -> -1 */
    napi_value v;
    napi_create_int32(environment, e == TA_NIL ? -1 : (int32_t)e, &v);
    return v;
}

static ta_status e_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    nd_host *host = (nd_host *)d;
    nd_chart *binding_chart = (nd_chart *)calloc(1, sizeof *binding_chart);
    if (!binding_chart) return TA_ERROR_NO_MEMORY;
    binding_chart->image = chart->image;
    binding_chart->chart = chart->chart;
    binding_chart->function = (napi_ref *)calloc(1, sizeof(napi_ref));
    napi_value info, exprs, data;
    napi_create_object(host->environment, &info);
    uint32_t n = ta_image_expression_count(binding_chart->image, binding_chart->chart);
    napi_create_array_with_length(host->environment, n, &exprs);
    for (uint32_t e = 0; e < n; e++) {
        ta_expression_info expression_info;
        napi_value x;
        napi_create_object(host->environment, &x);
        if (ta_image_expression(binding_chart->image, binding_chart->chart, e, &expression_info) ==
            TA_OK) {
            napi_set_named_property(host->environment, x, "source",
                                    str_value(host->environment, expression_info.source));
            napi_set_named_property(host->environment, x, "kind",
                                    u32_value(host->environment, expression_info.kind));
            napi_set_named_property(host->environment, x, "line",
                                    u32_value(host->environment, expression_info.line));
        }
        napi_set_element(host->environment, exprs, e, x);
    }
    uint32_t nd = ta_image_data_count(binding_chart->image, binding_chart->chart);
    napi_create_array_with_length(host->environment, nd, &data);
    for (uint32_t i = 0; i < nd; i++) {
        ta_data_info data_info;
        napi_value x;
        napi_create_object(host->environment, &x);
        if (ta_image_data(binding_chart->image, binding_chart->chart, i, &data_info) == TA_OK) {
            napi_set_named_property(host->environment, x, "id",
                                    str_value(host->environment, data_info.id));
            napi_set_named_property(host->environment, x, "expr",
                                    expression_argument(host->environment, data_info.expression));
        }
        napi_set_element(host->environment, data, i, x);
    }
    napi_set_named_property(
        host->environment, info, "name",
        str_value(host->environment,
                  ta_image_chart_source(binding_chart->image, binding_chart->chart)));
    napi_set_named_property(host->environment, info, "exprs", exprs);
    napi_set_named_property(host->environment, info, "data", data);
    napi_value r = ecmascript_call(host, NULL, "attach", 1, &info);
    if (r) binding_chart->ecmascript = make_ref(host->environment, r);
    *context = binding_chart;
    chart_add(host, binding_chart);
    return r ? TA_OK : TA_ERROR_INVALID;
}

static napi_value system_variables_value(nd_host *host,
                                         const ta_datamodel_system_variables *variables) {
    napi_value o, io;
    napi_create_object(host->environment, &o);
    napi_set_named_property(host->environment, o, "sessionid",
                            str_value(host->environment, variables->sessionid));
    napi_set_named_property(host->environment, o, "name",
                            str_value(host->environment, variables->name));
    napi_create_array_with_length(host->environment, variables->io_processor_count, &io);
    for (uint32_t i = 0; i < variables->io_processor_count; i++) {
        napi_value e;
        napi_create_object(host->environment, &e);
        napi_set_named_property(host->environment, e, "type",
                                str_value(host->environment, variables->io_processors[i].type));
        napi_set_named_property(host->environment, e, "location",
                                str_value(host->environment, variables->io_processors[i].location));
        napi_set_element(host->environment, io, i, e);
    }
    napi_set_named_property(host->environment, o, "ioprocessors", io);
    return o;
}

/* a session: create (init) or deserialize (state) */
static ta_status e_new(nd_host *host, void *chart_context,
                       const ta_datamodel_system_variables *variables, napi_value init,
                       napi_value state, uint64_t *out) {
    nd_chart *chart = (nd_chart *)chart_context;
    if (!chart || !chart->ecmascript) return TA_ERROR_INVALID;
    nd_instance *instance = instance_new(host, chart_context, variables, out);
    if (!instance) return TA_ERROR_NO_MEMORY;
    napi_value argv[4] = {get_ref(host->environment, chart->ecmascript),
                          system_variables_value(host, variables), state ? state : init,
                          get_ref(host->environment, instance->in_function)};
    napi_value r = ecmascript_call(host, instance, state ? "deserialize" : "create", 4, argv);
    if (!r) {
        map_put(host, instance->serial, 0);
        instance_free(host, (uint32_t)*out);
        return TA_ERROR_INVALID;
    }
    set_ref(host->environment, &instance->object, r);
    instance->datamodel_handle = *out += ECMASCRIPT_HANDLE;
    return TA_OK;
}

static ta_status e_create(void *d, void *chart_context,
                          const ta_datamodel_system_variables *variables, ta_payload init,
                          uint64_t *out) {
    nd_host *host = (nd_host *)d;
    ta_status status = e_new(host, chart_context, variables, payload_get(host, init), NULL, out);
    nd_release(host, init); /* ownership of the init payload passes to us */
    return status;
}

static napi_value e_vars(nd_host *host, nd_instance *instance) {
    napi_value o = get_ref(host->environment, instance->object);
    napi_value r = o ? ecmascript_call(host, instance, "vars", 1, &o) : NULL;
    return r ? r : undef(host->environment);
}

static void e_destroy(void *d, uint64_t handle) {
    nd_host *host = (nd_host *)d;
    nd_instance *instance = E_I(host, handle);
    if (!instance) return;
    if (!host->destroyed && !host->hibernating)
        set_ref(host->environment, &instance->context, e_vars(host, instance));
    nd_destroy(d, (uint64_t)(instance - host->instances) + 1); /* the grave, the serial map */
}

#define E_ENTER()                                                                                  \
    nd_host *host = (nd_host *)d;                                                                  \
    nd_instance *instance = E_I(host, handle);                                                     \
    if (!instance || !instance->object) return TA_ERROR_EXECUTION;                                 \
    instance->environment = environment;                                                           \
    napi_value self = get_ref(host->environment, instance->object)

static ta_status e_init_data(void *d, uint64_t handle, ta_datamodel_environment *environment,
                             uint32_t declaration) {
    E_ENTER();
    napi_value argv[2] = {self, u32_value(host->environment, declaration)};
    return truthy(host, ecmascript_call(host, instance, "initData", 2, argv)) ? TA_OK
                                                                              : TA_ERROR_EXECUTION;
}

static napi_value str_or_undef(napi_env environment, ta_string_view s) {
    return s.length ? str_value(environment, s) : undef(environment);
}

static ta_status e_bind_event(void *d, uint64_t handle, ta_datamodel_environment *environment,
                              const ta_event_view *view) {
    E_ENTER();
    napi_value o;
    napi_create_object(host->environment, &o);
    napi_set_named_property(host->environment, o, "name", str_value(host->environment, view->name));
    napi_set_named_property(host->environment, o, "type", u32_value(host->environment, view->type));
    napi_set_named_property(host->environment, o, "sendid",
                            str_or_undef(host->environment, view->sendid));
    napi_set_named_property(host->environment, o, "origin",
                            str_or_undef(host->environment, view->origin));
    napi_set_named_property(host->environment, o, "origintype",
                            str_or_undef(host->environment, view->origintype));
    napi_set_named_property(host->environment, o, "invokeid",
                            str_or_undef(host->environment, view->invokeid));
    napi_set_named_property(host->environment, o, "data", payload_get(host, view->data));
    if (view->size >= sizeof *view && view->raw.length)
        napi_set_named_property(host->environment, o, "raw",
                                str_value(host->environment, view->raw));
    napi_value argv[2] = {self, o};
    return ecmascript_call(host, instance, "bindEvent", 2, argv) ? TA_OK : TA_ERROR_EXECUTION;
}

static ta_status e_condition(void *d, uint64_t handle, ta_datamodel_environment *environment,
                             ta_expression expression, int *out) {
    E_ENTER();
    napi_value argv[2] = {self, u32_value(host->environment, expression)};
    napi_value r = ecmascript_call(host, instance, "cond", 2, argv);
    int32_t v = -1;
    if (r) napi_get_value_int32(host->environment, r, &v);
    if (v < 0) return TA_ERROR_EXECUTION;
    *out = v;
    return TA_OK;
}

/* string(): a string, or null = error.execution */
static ta_status e_str(nd_host *host, nd_instance *instance, const char *method, napi_value self,
                       ta_expression expression, ta_string_view *out) {
    napi_value argv[2] = {self, u32_value(host->environment, expression)};
    napi_value r = ecmascript_call(host, instance, method, 2, argv);
    napi_valuetype t = napi_null;
    if (r) napi_typeof(host->environment, r, &t);
    if (t != napi_string) return TA_ERROR_EXECUTION;
    return to_str(host, instance, r, out);
}

static ta_status e_string(void *d, uint64_t handle, ta_datamodel_environment *environment,
                          ta_expression expression, ta_string_view *out) {
    E_ENTER();
    return e_str(host, instance, "string", self, expression, out);
}

static ta_status e_log(void *d, uint64_t handle, ta_datamodel_environment *environment,
                       ta_expression expression, ta_string_view *out) {
    E_ENTER();
    return e_str(host, instance, "log", self, expression, out);
}

static ta_status e_assign(void *d, uint64_t handle, ta_datamodel_environment *environment,
                          ta_expression location, ta_expression value) {
    E_ENTER();
    napi_value argv[3] = {self, u32_value(host->environment, location),
                          u32_value(host->environment, value)};
    return truthy(host, ecmascript_call(host, instance, "assign", 3, argv)) ? TA_OK
                                                                            : TA_ERROR_EXECUTION;
}

static ta_status e_assign_string(void *d, uint64_t handle, ta_expression location,
                                 ta_string_view v) {
    ta_datamodel_environment *environment = NULL;
    E_ENTER();
    napi_value argv[3] = {self, u32_value(host->environment, location),
                          str_value(host->environment, v)};
    return truthy(host, ecmascript_call(host, instance, "assignString", 3, argv))
               ? TA_OK
               : TA_ERROR_EXECUTION;
}

static ta_status e_script(void *d, uint64_t handle, ta_datamodel_environment *environment,
                          ta_expression expression) {
    E_ENTER();
    napi_value argv[2] = {self, u32_value(host->environment, expression)};
    return truthy(host, ecmascript_call(host, instance, "script", 2, argv)) ? TA_OK
                                                                            : TA_ERROR_EXECUTION;
}

/* <foreach>: the items array is kept in the payload table (iter = its handle) */
static ta_status e_foreach_begin(void *d, uint64_t handle, ta_datamodel_environment *environment,
                                 ta_expression arr, uint64_t *iter, uint32_t *n) {
    E_ENTER();
    napi_value argv[2] = {self, u32_value(host->environment, arr)};
    napi_value items = ecmascript_call(host, instance, "foreachBegin", 2, argv);
    bool is_array = false;
    if (!items || napi_is_array(host->environment, items, &is_array) != napi_ok || !is_array)
        return TA_ERROR_EXECUTION;
    napi_get_array_length(host->environment, items, n);
    *iter = ECMASCRIPT_HANDLE + payload_new(host, items);
    return TA_OK;
}

static ta_status e_foreach_item(void *d, uint64_t handle, uint64_t iter, uint32_t i,
                                ta_expression item, ta_expression index) {
    ta_datamodel_environment *environment = NULL;
    E_ENTER();
    napi_value items =
        payload_get(host, iter >= ECMASCRIPT_HANDLE ? iter - ECMASCRIPT_HANDLE : iter);
    napi_value argv[5] = {self, items, u32_value(host->environment, i),
                          u32_value(host->environment, item),
                          expression_argument(host->environment, index)};
    return truthy(host, ecmascript_call(host, instance, "foreachItem", 5, argv))
               ? TA_OK
               : TA_ERROR_EXECUTION;
}

static void e_foreach_end(void *d, uint64_t handle, uint64_t iter) {
    (void)handle;
    nd_release(d, iter >= ECMASCRIPT_HANDLE ? iter - ECMASCRIPT_HANDLE : iter);
}

static ta_status e_capture(void *d, uint64_t handle, ta_datamodel_environment *environment,
                           const ta_datamodel_capture *capture, ta_payload *out) {
    E_ENTER();
    napi_value names, params;
    napi_create_array_with_length(host->environment, capture->namelist_count, &names);
    for (uint32_t i = 0; i < capture->namelist_count; i++)
        napi_set_element(host->environment, names, i,
                         u32_value(host->environment, capture->namelist[i]));
    napi_create_array_with_length(host->environment, capture->param_count, &params);
    for (uint32_t i = 0; i < capture->param_count; i++) {
        napi_value pair;
        napi_create_array_with_length(host->environment, 2, &pair);
        napi_set_element(host->environment, pair, 0,
                         str_value(host->environment, ta_image_string(instance->chart->image,
                                                                      capture->params[2 * i])));
        napi_set_element(host->environment, pair, 1,
                         u32_value(host->environment, capture->params[2 * i + 1]));
        napi_set_element(host->environment, params, i, pair);
    }
    napi_value argv[4] = {self, expression_argument(host->environment, capture->content), names,
                          params};
    napi_value r = ecmascript_call(host, instance, "capture", 4, argv), ok, v;
    if (!r || napi_get_named_property(host->environment, r, "ok", &ok) != napi_ok ||
        !truthy(host, ok))
        return TA_ERROR_EXECUTION;
    napi_get_named_property(host->environment, r, "v", &v);
    *out = payload_new(host, v);
    return TA_OK;
}

/* es.serialize(session): its state as plain values, or NULL with the reason kept
 * (what cannot be stored, named by the datamodel: storeError()) */
static napi_value ecmascript_state(nd_host *host, nd_instance *instance) {
    napi_value ecmascript = get_ref(host->environment, host->ecmascript), fn, r = NULL,
               self = get_ref(host->environment, instance->object);
    if (napi_get_named_property(host->environment, ecmascript, "serialize", &fn) != napi_ok)
        return NULL;
    nd_instance *outer = host->evaluating;
    host->evaluating = instance;
    napi_status status = napi_call_function(host->environment, ecmascript, fn, 1, &self, &r);
    host->evaluating = outer;
    if (status != napi_ok) {
        keep_error(host);
        return NULL;
    }
    return r;
}

static ta_status e_serialize(void *d, uint64_t handle, void *buffer, size_t capacity,
                             size_t *length) {
    nd_host *host = (nd_host *)d;
    nd_instance *instance = E_I(host, handle);
    *length = 0;
    if (!instance || !instance->object) return TA_ERROR_UNSUPPORTED;
    napi_value o = ecmascript_state(host, instance),
               bytes = o ? codec_call(host, "encode", o) : NULL;
    size_t n = 0;
    void *data = NULL;
    if (!bytes || !bytes_of(host->environment, bytes, &data, &n)) return TA_ERROR_UNSUPPORTED;
    *length = n;
    if (n > capacity) return TA_ERROR_NO_MEMORY;
    if (n) memcpy(buffer, data, n);
    return TA_OK;
}

static ta_status e_deserialize(void *d, void *chart_context,
                               const ta_datamodel_system_variables *variables, const void *buffer,
                               size_t length, uint64_t *out) {
    nd_host *host = (nd_host *)d;
    napi_value dec = codec_function(host, "decode"), ab, u8, global, o;
    if (!dec) return TA_ERROR_UNSUPPORTED;
    void *data = NULL;
    napi_create_arraybuffer(host->environment, length, &data, &ab);
    if (length) memcpy(data, buffer, length);
    napi_create_typedarray(host->environment, napi_uint8_array, length, ab, 0, &u8);
    napi_get_global(host->environment, &global);
    if (napi_call_function(host->environment, global, dec, 1, &u8, &o) != napi_ok) {
        take_exception(host, NULL);
        return TA_ERROR_INVALID;
    }
    return e_new(host, chart_context, variables, NULL, o, out);
}

static ta_status e_migrate(void *d, uint64_t handle, ta_datamodel_environment *environment,
                           ta_payload *bindings) {
    E_ENTER();
    napi_value r = ecmascript_call(host, instance, "migrate", 1, &self);
    instance->environment = NULL;
    *bindings = r ? payload_new(host, r) : 0;
    return r ? TA_OK : TA_ERROR_EXECUTION;
}

static const ta_datamodel_vtable e_vt = {sizeof(ta_datamodel_vtable),
                                         0,
                                         "ecmascript",
                                         e_attach,
                                         nd_detach,
                                         e_create,
                                         e_destroy,
                                         e_init_data,
                                         e_bind_event,
                                         NULL,
                                         e_condition,
                                         e_string,
                                         e_assign,
                                         e_assign_string,
                                         e_script,
                                         e_log,
                                         e_foreach_begin,
                                         e_foreach_item,
                                         e_foreach_end,
                                         e_capture,
                                         nd_retain,
                                         nd_release,
                                         NULL,
                                         e_serialize,
                                         e_deserialize,
                                         e_migrate,
                                         NULL, /* to_wire: links encode JS values in JS */
                                         NULL,
                                         nd_payload_serialize,
                                         nd_payload_deserialize};

/* ---- hosts ------------------------------------------------------------------------------ */

static void host_clear(napi_env environment, nd_host *host) {
    host->environment = environment;
    for (uint32_t i = 0; i < host->instance_count; i++)
        if (host->instances[i].live) instance_free(host, i + 1);
    free(host->instances);
    for (uint32_t i = 0; i < host->payload_count; i++)
        if (host->refs[i]) napi_delete_reference(environment, host->payloads[i]);
    free(host->payloads);
    free(host->refs);
    for (uint32_t i = 0; i < host->image_count; i++)
        set_ref(environment, &host->images[i], NULL);
    free(host->images);
    for (uint32_t i = 0; i < host->grave_count; i++) {
        set_ref(environment, &host->graves[i].context, NULL);
        set_ref(environment, &host->graves[i].done_config, NULL);
    }
    free(host->graves);
    for (uint32_t i = 0; i < host->error_count; i++)
        set_ref(environment, &host->errors[i].error, NULL);
    free(host->errors);
    free(host->charts);
    free(host->keys);
    free(host->values);
    set_ref(environment, &host->resolver, NULL);
    set_ref(environment, &host->ecmascript, NULL);
    set_ref(environment, &host->codec, NULL);
    set_ref(environment, &host->store_error, NULL);
    free(host->cached_encoding);
    memset(host, 0, sizeof *host);
    host->destroyed = 1;
}

/* Release references now: callbacks can close over the JS owner of this handle,
 * so waiting for finalization would keep both alive. Only the tombstone stays
 * until JS drops it, allowing repeated destroy and rejecting later operations. */
static void host_destroy(napi_env environment, nd_host *host) {
    if (host->destroyed) return;
    host->environment = environment;
    host->destroyed = 1;
    if (host->host) ta_host_free(host->host);
    host_clear(environment, host);
}

static void host_finalize(napi_env environment, void *data, void *hint) {
    (void)hint;
    nd_host *host = (nd_host *)data;
    host_destroy(environment, host);
    free(host);
}

static nd_host *get_host(napi_env environment, napi_value v) {
    void *p = NULL;
    if (napi_get_value_external(environment, v, &p) != napi_ok || !p) {
        napi_throw_type_error(environment, NULL, "expected a host handle");
        return NULL;
    }
    nd_host *host = (nd_host *)p;
    if (!host->host) {
        napi_throw_error(environment, NULL, "the actor system was destroyed");
        return NULL;
    }
    host->environment = environment;
    return host;
}

static uint32_t get_u32(napi_env environment, napi_value v) {
    uint32_t x = 0;
    napi_get_value_uint32(environment, v, &x);
    return x;
}

static uint64_t get_session(napi_env environment, napi_value v) {
    double d = 0;
    napi_get_value_double(environment, v, &d);
    return d > 0 ? (uint64_t)d : 0;
}

static napi_value num(napi_env environment, double d) {
    napi_value v;
    napi_create_double(environment, d, &v);
    return v;
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

/* createHost(resolver, codec, base, es) -> host: the datamodels ("xstate" for
 * machines, "ecmascript" for SCXML documents: ecmascript = bindings/shared/ecmascript.mjs)
 * and the ta_host */
/* A host object from (resolver, codec, base, es); its ta_host config in *c (datamodels in b) */
static nd_host *host_alloc(napi_env environment, napi_value *argv, ta_host_config *config,
                           ta_datamodel_binding *binding) {
    nd_host *host = (nd_host *)calloc(1, sizeof *host);
    if (!host) return NULL;
    host->environment = environment;
    host->resolver = make_ref(environment, argv[0]);
    host->codec = make_ref(environment, argv[1]);
    host->ecmascript = make_ref(environment, argv[3]);
    size_t bl = 0; /* argv[2]: the system id; h->base = tinyactors://<id>/ */
    char id[TA_ADDRESS_SYSTEM_MAX + 1];
    napi_get_value_string_utf8(environment, argv[2], id, sizeof id, &bl);
    snprintf(host->base, sizeof host->base, "tinyactors://%.*s/", (int)bl, id);
    binding[0] = (ta_datamodel_binding){&nd_vt, host};
    binding[1] = (ta_datamodel_binding){&e_vt, host};
    memset(config, 0, sizeof *config);
    config->size = sizeof *config;
    config->system_id = (ta_string_view){host->base + 13, (uint32_t)bl};
    config->system.size = sizeof config->system;
    config->system.ticks_per_second = 1000; /* ms */
    config->system.max_actors = 64;
    config->system.max_events = 256;
    config->system.max_timers = 64;
    config->system.string_bytes = 32768;
    config->datamodels = binding; /* "xstate" (machines), "ecmascript" (SCXML documents) */
    config->datamodel_count = 2;
    config->allocator = ta_runtime_malloc();
    return host;
}

static napi_value js_create_host(napi_env environment, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4], result;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    napi_valuetype t = napi_undefined;
    if (argc) napi_typeof(environment, argv[0], &t);
    if (t != napi_function || argc < 4) {
        napi_throw_type_error(environment, NULL, "createHost(resolver, codec, base, es)");
        return NULL;
    }
    ta_host_config config;
    ta_datamodel_binding binding[2];
    nd_host *host = host_alloc(environment, argv, &config, binding);
    if (!host || ta_host_new(&config, &host->host) != TA_OK) {
        if (host) host_finalize(environment, host, NULL);
        napi_throw_error(environment, NULL, "cannot create the actor system");
        return NULL;
    }
    napi_create_external(environment, host, host_finalize, NULL, &result);
    return result;
}

/* addImage(host, image) -> segment: a new segment (its charts: the current versions) */
static napi_value js_add_image(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    nd_image *image = host ? get_image(environment, argv[1]) : NULL;
    if (!image) return NULL;
    uint32_t segment = 0;
    ta_status status = ta_host_add_image(host->host, &image->image, &segment);
    if (status != TA_OK) {
        napi_throw_error(environment, NULL, "cannot create the actor system");
        return NULL;
    }
    if (segment >= host->image_capacity) {
        uint32_t c = host->image_capacity ? host->image_capacity * 2 : 8;
        while (c <= segment)
            c *= 2;
        host->images = (napi_ref *)realloc(host->images, sizeof(napi_ref) * c);
        memset(host->images + host->image_capacity, 0,
               sizeof(napi_ref) * (c - host->image_capacity));
        host->image_capacity = c;
    }
    host->images[segment] = make_ref(environment, argv[1]); /* until TA_HOST_EFFECT_SEGMENT_FREED */
    if (segment >= host->image_count) host->image_count = segment + 1;
    return num(environment, segment);
}

/* chart(host, name) -> [segment, chart] of the current version, or null */
static napi_value js_chart(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], r;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t nl = 0;
    char *name = get_string(environment, argv[1], &nl);
    uint32_t segment = 0, chart = 0;
    ta_status status =
        name ? ta_host_chart(host->host, (ta_string_view){name, (uint32_t)nl}, &segment, &chart)
             : TA_ERROR_INVALID;
    free(name);
    if (status != TA_OK) return null_value(environment);
    napi_create_array_with_length(environment, 2, &r);
    napi_set_element(environment, r, 0, num(environment, segment));
    napi_set_element(environment, r, 1, num(environment, chart));
    return r;
}

/* spawn(host, segment, chart, input, params, name) -> session; params (SCXML
 * documents: <data> overrides) are the init payload as is, else { input }; name: the
 * actor's systemID (addressable as tinyactors://<system>/@<name>) */
static napi_value js_spawn(napi_env environment, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6], init;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 3 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    napi_valuetype pt = napi_undefined;
    if (argc > 4) napi_typeof(environment, argv[4], &pt);
    if (pt != napi_undefined) {
        init = argv[4];
    } else {
        napi_create_object(environment, &init);
        if (argc > 3) napi_set_named_property(environment, init, "input", argv[3]);
    }
    ta_payload payload = payload_new(host, init);
    uint64_t session = 0;
    size_t nl = 0;
    napi_valuetype nt = napi_undefined;
    if (argc > 5) napi_typeof(environment, argv[5], &nt);
    char *name = nt == napi_string ? get_string(environment, argv[5], &nl) : NULL;
    ta_status status =
        ta_host_spawn(host->host, get_u32(environment, argv[1]), get_u32(environment, argv[2]),
                      (ta_string_view){name, (uint32_t)nl}, payload, &session);
    free(name);
    if (status != TA_OK) {
        nd_release(host, payload);
        napi_throw_error(environment, NULL, "cannot spawn the actor");
        return NULL;
    }
    return num(environment, (double)session);
}

static ta_status host_send(nd_host *host, uint64_t session, ta_event_in *event) {
    ta_status status = ta_host_send(host->host, session, event);
    if (status != TA_OK && event->data)
        nd_release(host, event->data); /* a failed send leaves it with us */
    return status;
}

/* send(host, session, type, event) -> bool */
static napi_value js_send(napi_env environment, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 4 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t tl;
    char *type = get_string(environment, argv[2], &tl);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){type ? type : "", (uint32_t)(type ? tl : 0)};
    event.data = payload_new(host, argv[3]);
    ta_status status = host_send(host, get_session(environment, argv[1]), &event);
    free(type);
    return bool_value(environment, status == TA_OK);
}

/* sendMany(host, sessions[], types[], events[]) -> how many were delivered (batched:
 * ta_host_send_many, one ta_system_send_many per segment run) */
static napi_value js_send_many(napi_env environment, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 4 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    uint32_t n = 0;
    napi_get_array_length(environment, argv[1], &n);
    uint64_t *sessions = (uint64_t *)malloc(sizeof(uint64_t) * (n ? n : 1));
    ta_event_in *events = (ta_event_in *)calloc(n ? n : 1, sizeof *events);
    char **types = (char **)calloc(n ? n : 1, sizeof(char *));
    for (uint32_t i = 0; i < n; i++) {
        napi_value v, t, ev;
        napi_get_element(environment, argv[1], i, &v);
        napi_get_element(environment, argv[2], i, &t);
        napi_get_element(environment, argv[3], i, &ev);
        sessions[i] = get_session(environment, v);
        size_t tl = 0;
        types[i] = get_string(environment, t, &tl);
        events[i].size = sizeof events[i];
        events[i].name = (ta_string_view){types[i] ? types[i] : "", (uint32_t)(types[i] ? tl : 0)};
        events[i].data = payload_new(host, ev);
    }
    uint32_t ok = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t k =
            ta_host_send_many(host->host, sessions + i, events + i, n - i); /* accepted prefix */
        ok += k;
        i += k;
        if (i < n) { /* events[i] failed (e.g. its session ended): it is still ours */
            nd_release(host, events[i].data);
            i++;
        }
    }
    for (uint32_t i = 0; i < n; i++)
        free(types[i]);
    free(types);
    free(events);
    free(sessions);
    return num(environment, ok);
}

/* ---- the router (docs/protocol.md, Routing): peers are integer handles owned by the
 * ta_host; links and other transports move envelopes in (deliverEnvelope) and out
 * (run().peers, run().peerErrors). */

/* a named string property of `o` (NULL if absent or not a string); *length set */
static char *prop_string(napi_env environment, napi_value o, const char *name, size_t *length) {
    napi_value v;
    napi_valuetype t = napi_undefined;
    *length = 0;
    if (napi_get_named_property(environment, o, name, &v) != napi_ok) return NULL;
    napi_typeof(environment, v, &t);
    return t == napi_string ? get_string(environment, v, length) : NULL;
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
    nd_host *host = argc >= 4 ? get_host(environment, argv[0]) : NULL;
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
    nd_host *host = argc >= 3 ? get_host(environment, argv[0]) : NULL;
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
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    return bool_value(environment,
                      ta_host_peer_remove(host->host, get_u32(environment, argv[1])) == TA_OK);
}

/* peerReach(host, peer, reach[]) -> bool */
static napi_value js_peer_reach(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 3 ? get_host(environment, argv[0]) : NULL;
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
    nd_host *host = argc >= 3 ? get_host(environment, argv[0]) : NULL;
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
    nd_host *host = argc >= 4 ? get_host(environment, argv[0]) : NULL;
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

/* deliverEnvelope(host, peer, { target, name, data, origin, origintype, sendid, msgid,
 * hops, dedup }) -> outcome (0 delivered, 1 relayed, 2 duplicate, 3 refused; -1 invalid).
 * peer 0: from this host's own code (no checks). */
static napi_value js_deliver_envelope(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 3 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    static const char *const keys[] = {"target", "name",  "origin", "origintype",
                                       "sendid", "msgid", "dedup"};
    char *s[7];
    size_t l[7];
    for (int i = 0; i < 7; i++)
        s[i] = prop_string(environment, argv[2], keys[i], &l[i]);
#define V(i) ((ta_string_view){s[i] ? s[i] : "", s[i] ? (uint32_t)l[i] : 0})
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = V(0);
    envelope.name = V(1);
    envelope.origin = V(2);
    envelope.origintype = V(3);
    envelope.sendid = V(4);
    envelope.msgid = V(5);
    envelope.deduplication_key = V(6);
#undef V
    napi_value hv, dv;
    napi_valuetype t = napi_undefined;
    if (napi_get_named_property(environment, argv[2], "hops", &hv) == napi_ok) {
        napi_typeof(environment, hv, &t);
        if (t == napi_number) envelope.hops = get_u32(environment, hv);
    }
    t = napi_undefined;
    if (napi_get_named_property(environment, argv[2], "data", &dv) == napi_ok)
        napi_typeof(environment, dv, &t);
    envelope.datamodel = 0; /* both datamodels share the payload table */
    envelope.data = t == napi_undefined ? 0 : payload_new(host, dv);
    uint32_t outcome = TA_ROUTE_REFUSED;
    ta_status status =
        ta_host_deliver(host->host, get_u32(environment, argv[1]), &envelope, &outcome);
    /* the payload moves only when delivered or relayed */
    if (envelope.data && (status != TA_OK || outcome > TA_ROUTE_RELAYED))
        nd_release(host, envelope.data);
    for (int i = 0; i < 7; i++)
        free(s[i]);
    return num(environment, status == TA_OK ? (double)outcome : -1);
}

/* peerError(host, peer, origin, msgid, sendid, reason) -> bool: an `err` frame */
static napi_value js_peer_error(napi_env environment, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 6 ? get_host(environment, argv[0]) : NULL;
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
    nd_host *host = argc >= 3 ? get_host(environment, argv[0]) : NULL;
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
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
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

/* the session of the host-spawned ancestor of `session` (itself if gone) */
static uint64_t root_session(nd_host *host, uint64_t session) {
    uint32_t segment, actor;
    if (!session || ta_host_locate(host->host, session, &segment, &actor) != TA_OK) return session;
    ta_system *system = ta_host_system(host->host, segment);
    uint64_t root = session;
    for (int depth = 0; depth < 64 && actor; depth++) {
        ta_actor_info actor_info;
        memset(&actor_info, 0, sizeof actor_info);
        actor_info.size = sizeof actor_info;
        if (ta_actor_info_get(system, actor, &actor_info) != TA_OK) break;
        root = actor_info.serial;
        actor = actor_info.parent;
    }
    return root;
}

/* append to a result list, made on first use (most runs produce few kinds of effects:
 * on wasm every napi call crosses into JS) */
static void push(napi_env environment, napi_value *arr, uint32_t *n, napi_value v) {
    if (!*arr) napi_create_array(environment, arr);
    napi_set_element(environment, *arr, (*n)++, v);
}

/* run(host, budget) -> { status, deadline, used, logs, done, ended, peers, peerErrors,
 *                        errors, wakes, requests, migrated, jobs, freed } (lists only when
 *                        not empty) */
static napi_value js_run(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], out, v, logs = NULL, done = NULL, sends = NULL, perrs = NULL, errors = NULL,
                                wakes = NULL, requests = NULL, migrated = NULL, jobs = NULL,
                                freed = NULL, graves = NULL;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 1 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    uint32_t budget = argc > 1 ? get_u32(environment, argv[1]) : 100000;
    uint32_t nl = 0, nd = 0, ns = 0, npe = 0, ne = 0, nw = 0, nr = 0, nm = 0, nj = 0, nf = 0;
    ta_run result;
    for (int round = 0;; round++) {
        result = ta_host_run(host->host, budget);
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(host->host, &effects);
        for (uint32_t i = 0; i < n; i++) {
            const ta_host_effect *effect = &effects[i];
            napi_value e = NULL;
            if (effect->kind != TA_EFFECT_WAKE && effect->kind != TA_HOST_EFFECT_SEGMENT_FREED)
                napi_create_object(environment, &e);
            switch (effect->kind) {
            case TA_EFFECT_LOG:
                napi_set_named_property(environment, e, "session",
                                        num(environment, (double)effect->root));
                napi_set_named_property(environment, e, "label",
                                        str_value(environment, effect->auxiliary));
                napi_set_named_property(environment, e, "message",
                                        str_value(environment, effect->event));
                push(environment, &logs, &nl, e);
                break;
            case TA_EFFECT_DONE:
                napi_set_named_property(environment, e, "session",
                                        num(environment, (double)effect->session));
                napi_set_named_property(environment, e, "state",
                                        str_value(environment, effect->auxiliary));
                napi_set_named_property(environment, e, "output",
                                        payload_get(host, effect->payload));
                push(environment, &done, &nd, e);
                break;
            case TA_HOST_EFFECT_PEER: /* an envelope for a transport (links, store links...) */
                napi_set_named_property(environment, e, "peer", num(environment, effect->peer));
                napi_set_named_property(environment, e, "target",
                                        str_value(environment, effect->target));
                napi_set_named_property(environment, e, "name",
                                        str_value(environment, effect->event));
                napi_set_named_property(environment, e, "data", payload_get(host, effect->payload));
                napi_set_named_property(environment, e, "origin",
                                        str_value(environment, effect->auxiliary));
                napi_set_named_property(environment, e, "sendid",
                                        str_value(environment, effect->sendid));
                napi_set_named_property(environment, e, "msgid",
                                        str_value(environment, effect->msgid));
                napi_set_named_property(environment, e, "hops", num(environment, effect->flags));
                push(environment, &sends, &ns, e);
                break;
            case TA_HOST_EFFECT_PEER_ERROR: /* an `err` back to a peer */
                napi_set_named_property(environment, e, "peer", num(environment, effect->peer));
                napi_set_named_property(environment, e, "origin",
                                        str_value(environment, effect->auxiliary));
                napi_set_named_property(environment, e, "msgid",
                                        str_value(environment, effect->msgid));
                napi_set_named_property(environment, e, "sendid",
                                        str_value(environment, effect->sendid));
                napi_set_named_property(environment, e, "reason", num(environment, effect->token));
                push(environment, &perrs, &npe, e);
                break;
            case TA_EFFECT_WAKE: /* hibernated with work, or wanted by a migration job */
                push(environment, &wakes, &nw, num(environment, (double)effect->session));
                break;
            case TA_EFFECT_REQUEST:
                napi_set_named_property(environment, e, "segment",
                                        num(environment, effect->segment));
                napi_set_named_property(environment, e, "token", num(environment, effect->token));
                napi_set_named_property(environment, e, "src",
                                        str_value(environment, effect->auxiliary));
                push(environment, &requests, &nr, e);
                break;
            case TA_EFFECT_ERROR: {
                char buffer[64];
                snprintf(buffer, sizeof buffer, "actor fault %u", effect->flags);
                napi_value message, err;
                napi_create_string_utf8(environment, buffer, NAPI_AUTO_LENGTH, &message);
                napi_create_error(environment, NULL, message, &err);
                napi_set_named_property(environment, e, "session",
                                        num(environment, (double)effect->root));
                napi_set_named_property(environment, e, "error", err);
                push(environment, &errors, &ne, e);
                break;
            }
            case TA_HOST_EFFECT_MIGRATED:
                napi_set_named_property(environment, e, "session",
                                        num(environment, (double)effect->session));
                napi_set_named_property(environment, e, "segment",
                                        num(environment, effect->segment));
                napi_set_named_property(environment, e, "chart", num(environment, effect->chart));
                napi_set_named_property(environment, e, "job", num(environment, effect->job));
                push(environment, &migrated, &nm, e);
                break;
            case TA_HOST_EFFECT_JOB_DONE: {
                napi_value left;
                uint32_t k = ta_host_job_pending(host->host, effect->job, NULL, 0);
                uint64_t *buffer = (uint64_t *)malloc(sizeof(uint64_t) * (k ? k : 1));
                k = ta_host_job_pending(host->host, effect->job, buffer, k);
                napi_create_array_with_length(environment, k, &left);
                for (uint32_t j = 0; j < k; j++)
                    napi_set_element(environment, left, j, num(environment, (double)buffer[j]));
                free(buffer);
                napi_set_named_property(environment, e, "job", num(environment, effect->job));
                napi_set_named_property(environment, e, "migrated",
                                        num(environment, effect->migrated));
                napi_set_named_property(environment, e, "failed", num(environment, effect->failed));
                napi_set_named_property(environment, e, "pending", left);
                push(environment, &jobs, &nj, e);
                break;
            }
            case TA_HOST_EFFECT_SEGMENT_FREED: /* its image can go (and its chart contexts went) */
                if (effect->segment < host->image_count)
                    set_ref(environment, &host->images[effect->segment], NULL);
                push(environment, &freed, &nf, num(environment, effect->segment));
                break;
            default:
                break;
            }
        }
        ta_host_effects_ack(host->host);
        if (result.status == TA_RUN_EFFECTS_FULL && round < 8) continue;
        break;
    }
    /* sessions that ended during this run: their last context (after a
     * done.state.* event, the final configuration too) */
    for (uint32_t i = 0, ng = 0; i < host->grave_count; i++) {
        napi_value g;
        napi_create_object(environment, &g);
        napi_set_named_property(environment, g, "session",
                                num(environment, (double)host->graves[i].serial));
        napi_set_named_property(environment, g, "context",
                                get_ref(environment, host->graves[i].context));
        napi_set_named_property(environment, g, "config",
                                host->graves[i].done_config
                                    ? get_ref(environment, host->graves[i].done_config)
                                    : undef(environment));
        push(environment, &graves, &ng, g);
        set_ref(environment, &host->graves[i].context, NULL);
        set_ref(environment, &host->graves[i].done_config, NULL);
    }
    host->grave_count = 0;
    for (uint32_t i = 0; i < host->error_count; i++) { /* exceptions in user code */
        napi_value e;
        napi_create_object(environment, &e);
        napi_set_named_property(
            environment, e, "session",
            num(environment, (double)root_session(host, host->errors[i].serial)));
        napi_set_named_property(environment, e, "error",
                                get_ref(environment, host->errors[i].error));
        push(environment, &errors, &ne, e);
        set_ref(environment, &host->errors[i].error, NULL);
    }
    host->error_count = 0;
    static const char *const names[] = {"idle",      "yielded", "effects",
                                        "exhausted", "fault",   "effects"};
    napi_create_object(environment, &out);
    napi_create_string_utf8(environment, names[result.status < 6 ? result.status : 4],
                            NAPI_AUTO_LENGTH, &v);
    napi_set_named_property(environment, out, "status", v);
    napi_set_named_property(
        environment, out, "deadline",
        num(environment, result.next_deadline == UINT64_MAX ? -1 : (double)result.next_deadline));
    napi_set_named_property(environment, out, "used", num(environment, result.used));
    const char *keys[] = {"logs",  "done",     "ended",    "peers", "peerErrors", "errors",
                          "wakes", "requests", "migrated", "jobs",  "freed"};
    napi_value lists[] = {logs,  done,     graves,   sends, perrs, errors,
                          wakes, requests, migrated, jobs,  freed};
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++)
        if (lists[i]) napi_set_named_property(environment, out, keys[i], lists[i]);
    return out;
}

static napi_value js_advance(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    double ms = 0;
    napi_get_value_double(environment, argv[1], &ms);
    ta_host_advance_time(host->host, ms > 0 ? (ta_time)ms : 0);
    return NULL;
}

/* config(host, session) -> active state ids */
static napi_value js_config(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], arr;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    napi_create_array(environment, &arr);
    uint32_t segment, actor;
    if (ta_host_locate(host->host, get_session(environment, argv[1]), &segment, &actor) != TA_OK)
        return arr;
    ta_system *system = ta_host_system(host->host, segment);
    ta_actor_info actor_info;
    memset(&actor_info, 0, sizeof actor_info);
    actor_info.size = sizeof actor_info;
    ta_host_segment_info segment_info;
    segment_info.size = sizeof segment_info;
    if (ta_actor_info_get(system, actor, &actor_info) != TA_OK ||
        ta_host_segment_info_get(host->host, segment, &segment_info) != TA_OK)
        return arr;
    uint32_t states[512];
    uint32_t n = ta_actor_config(system, actor, states, 512);
    for (uint32_t i = 0; i < n && i < 512; i++)
        napi_set_element(environment, arr, i,
                         str_value(environment, ta_image_state_name(segment_info.image,
                                                                    actor_info.chart, states[i])));
    return arr;
}

static napi_value js_context(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    nd_instance *instance = I(host, map_get(host, get_session(environment, argv[1])));
    if (instance && instance->object)
        return e_vars(host, instance); /* an SCXML document: its variables */
    return instance ? get_ref(environment, instance->context) : undef(environment);
}

static napi_value js_terminate(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    return bool_value(environment,
                      ta_host_terminate(host->host, get_session(environment, argv[1])) == TA_OK);
}

/* stats(host) -> { actors, hibernated, sessions, payloads, segments, routed, migrated, retired,
 *                  versions: [{ segment, actors, maxActors, current }] } */
static napi_value js_stats(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out, versions;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 1 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    ta_host_stats state;
    state.size = sizeof state;
    ta_host_stats_get(host->host, &state);
    napi_create_object(environment, &out);
    napi_set_named_property(environment, out, "actors", num(environment, state.actors));
    napi_set_named_property(environment, out, "hibernated", num(environment, state.hibernated));
    napi_set_named_property(environment, out, "sessions", num(environment, host->session_count));
    napi_set_named_property(environment, out, "payloads", num(environment, host->live_payloads));
    napi_set_named_property(environment, out, "segments", num(environment, state.segments));
    napi_set_named_property(environment, out, "routed", num(environment, (double)state.routed));
    napi_set_named_property(environment, out, "migrated", num(environment, (double)state.migrated));
    napi_set_named_property(environment, out, "retired", num(environment, (double)state.retired));
    napi_create_array(environment, &versions);
    uint32_t k = 0, nseg = ta_host_segment_count(host->host);
    for (uint32_t segment = 0; segment < nseg; segment++) {
        ta_host_segment_info segment_info;
        segment_info.size = sizeof segment_info;
        if (ta_host_segment_info_get(host->host, segment, &segment_info) != TA_OK ||
            !segment_info.live)
            continue;
        ta_system_stats stats;
        stats.size = sizeof stats;
        ta_system_stats_get(ta_host_system(host->host, segment), &stats);
        napi_value o;
        napi_create_object(environment, &o);
        napi_set_named_property(environment, o, "segment", num(environment, segment));
        napi_set_named_property(environment, o, "actors", num(environment, segment_info.actors));
        napi_set_named_property(environment, o, "maxActors",
                                num(environment, stats.actors_capacity));
        napi_set_named_property(environment, o, "hibernated", num(environment, stats.hibernated));
        napi_set_named_property(environment, o, "current",
                                bool_value(environment, segment_info.current > 0));
        napi_set_element(environment, versions, k++, o);
    }
    napi_set_named_property(environment, out, "versions", versions);
    return out;
}

/* sweep(host, max) -> sessions idle since the previous sweep, hibernatable now */
static napi_value js_sweep(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], arr;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 1 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    uint32_t max = argc > 1 ? get_u32(environment, argv[1]) : 1024;
    ta_host_stats stats;
    stats.size = sizeof stats;
    ta_host_stats_get(host->host, &stats);
    uint32_t capacity = max < stats.actors ? max : stats.actors;
    uint64_t *out = (uint64_t *)malloc(sizeof(uint64_t) * (capacity ? capacity : 1));
    uint32_t n = ta_host_sweep(host->host, UINT32_MAX, out, capacity); /* one full revolution */
    napi_create_array_with_length(environment, n, &arr);
    for (uint32_t i = 0; i < n; i++)
        napi_set_element(environment, arr, i, num(environment, (double)out[i]));
    free(out);
    return arr;
}

static napi_value bytes_value(napi_env environment, const void *p, size_t n) {
    void *data = NULL;
    napi_value ab, r;
    napi_create_arraybuffer(environment, n, &data, &ab);
    if (n) memcpy(data, p, n);
    napi_create_typedarray(environment, napi_uint8_array, n, ab, 0, &r);
    return r;
}

static int bytes_of(napi_env environment, napi_value v, void **data, size_t *n) {
    napi_typedarray_type tt;
    size_t offset = 0;
    napi_value ab;
    return napi_get_typedarray_info(environment, v, &tt, n, data, &ab, &offset) == napi_ok &&
           tt == napi_uint8_array;
}

/* hibernate(host, session) -> Uint8Array (the blob), or null if it cannot be now */
static napi_value js_hibernate(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    uint64_t session = get_session(environment, argv[1]);
    size_t capacity = 4096, length = 0;
    unsigned char *buffer = (unsigned char *)malloc(capacity);
    host->hibernating = 1;
    set_ref(environment, &host->store_error, NULL);
    ta_status status = ta_host_hibernate(host->host, session, buffer, capacity, &length);
    if (status == TA_ERROR_NO_MEMORY && length > capacity) {
        buffer = (unsigned char *)realloc(buffer, length);
        capacity = length;
        status = ta_host_hibernate(host->host, session, buffer, capacity, &length);
    }
    host->hibernating = 0;
    napi_value r =
        status == TA_OK ? bytes_value(environment, buffer, length) : null_value(environment);
    free(buffer);
    return r;
}

/* wake(host, session, blob) -> bool */
static napi_value js_wake(napi_env environment, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 3 ? get_host(environment, argv[0]) : NULL;
    void *data = NULL;
    size_t n = 0;
    if (!host || !bytes_of(environment, argv[2], &data, &n)) return NULL;
    return bool_value(
        environment, ta_host_wake(host->host, get_session(environment, argv[1]), data, n) == TA_OK);
}

/* storeError(host) -> why the last hibernate/exportActor could not store a session
 * (the datamodel's exception, naming what cannot be stored), or null */
static napi_value js_store_error(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    napi_value r =
        host->store_error ? get_ref(environment, host->store_error) : null_value(environment);
    set_ref(environment, &host->store_error, NULL);
    return r;
}

/* exportActor(host, session) -> { blob, segment, chart }, or null if it cannot leave now
 * (storeError says why when a value cannot be stored) */
static napi_value js_export_actor(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    uint64_t session = get_session(environment, argv[1]);
    uint32_t segment = 0, actor = 0;
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    if (ta_host_locate(host->host, session, &segment, &actor) != TA_OK ||
        ta_actor_info_get(ta_host_system(host->host, segment), actor, &actor_info) != TA_OK)
        return null_value(environment);
    set_ref(environment, &host->store_error, NULL);
    size_t capacity = 4096, length = 0;
    unsigned char *buffer = (unsigned char *)malloc(capacity);
    ta_status status = buffer ? ta_host_export_actor(host->host, session, buffer, capacity, &length)
                              : TA_ERROR_NO_MEMORY;
    if (status == TA_ERROR_NO_MEMORY && buffer && length > capacity) {
        unsigned char *nb = (unsigned char *)realloc(buffer, length);
        if (nb) {
            buffer = nb;
            status = ta_host_export_actor(host->host, session, buffer, length, &length);
        }
    }
    napi_value r;
    if (status == TA_OK) {
        napi_create_object(environment, &r);
        napi_set_named_property(environment, r, "blob", bytes_value(environment, buffer, length));
        napi_set_named_property(environment, r, "segment", u32_value(environment, segment));
        napi_set_named_property(environment, r, "chart", u32_value(environment, actor_info.chart));
    } else {
        r = null_value(environment);
    }
    free(buffer);
    return r;
}

/* adopt(host, segment, blob, exportedAt, now) -> session: the blob's delayed sends keep
 * the time they had left at export (rebased from the exporter's clock to ours) */
static napi_value js_adopt(napi_env environment, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 5 ? get_host(environment, argv[0]) : NULL;
    void *data = NULL;
    size_t n = 0;
    if (!host || !bytes_of(environment, argv[2], &data, &n)) return NULL;
    double from = 0, to = 0;
    napi_get_value_double(environment, argv[3], &from);
    napi_get_value_double(environment, argv[4], &to);
    void *copy = malloc(n ? n : 1); /* rebased in place: not the caller's bytes */
    if (!copy) return NULL;
    if (n) memcpy(copy, data, n);
    uint64_t session = 0;
    ta_status status =
        ta_blob_rebase(copy, n, from > 0 ? (ta_time)from : 0, to > 0 ? (ta_time)to : 0);
    if (status == TA_OK)
        status = ta_host_adopt(host->host, get_u32(environment, argv[1]), copy, n, &session);
    free(copy);
    if (status != TA_OK) {
        napi_throw_error(environment, NULL,
                         status == TA_ERROR_STATE     ? "the actor is already in this system"
                         : status == TA_ERROR_INVALID ? "not an exported actor of this machine"
                                                      : "cannot adopt the actor");
        return NULL;
    }
    return num(environment, (double)session);
}

/* migrate(host, chartName, session, wake, timeoutMs) -> job id */
static napi_value js_migrate(napi_env environment, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 5 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t nl = 0;
    char *name = get_string(environment, argv[1], &nl);
    bool wake = false;
    napi_get_value_bool(environment, argv[3], &wake);
    double timeout = 0;
    napi_get_value_double(environment, argv[4], &timeout);
    uint32_t job = 0;
    ta_status status =
        ta_host_migrate(host->host, (ta_string_view){name ? name : "", (uint32_t)(name ? nl : 0)},
                        get_session(environment, argv[2]), wake ? TA_HOST_MIGRATE_WAKE : 0,
                        timeout > 0 ? (ta_time)timeout : 0, &job);
    free(name);
    if (status != TA_OK) {
        napi_throw_error(environment, NULL, "cannot start the migration");
        return NULL;
    }
    return num(environment, job);
}

/* resume(host, segment, request, chartOrMinusOne): answer a TA_EFFECT_REQUEST (an invoke
 * whose src is not a chart of the image) with a chart index, or -1 for an error */
static napi_value js_resume(napi_env environment, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 4 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    double chart = -1;
    napi_get_value_double(environment, argv[3], &chart);
    ta_status status = ta_host_resume(
        host->host, get_u32(environment, argv[1]), get_u32(environment, argv[2]),
        chart >= 0 ? TA_OK : TA_ERROR_INVALID, chart >= 0 ? (ta_payload)chart + 1 : 0);
    return bool_value(environment, status == TA_OK);
}

static napi_value js_destroy(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *p = NULL;
    if (argc >= 1 && napi_get_value_external(environment, argv[0], &p) == napi_ok && p)
        host_destroy(environment, (nd_host *)p);
    return NULL;
}

/* ---- system images ------------------------------------------------------------------- */

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

/* compileSystem(manifest, files) -> image: a <system> manifest (charts, boot actors) */
static napi_value js_compile_system(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], result = NULL;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    uint32_t nf = 0;
    bool is_array = false;
    if (argc > 1 && napi_is_array(environment, argv[1], &is_array) == napi_ok && is_array)
        napi_get_array_length(environment, argv[1], &nf);
    size_t ml = 0;
    char *manifest = argc ? get_string(environment, argv[0], &ml) : NULL;
    ta_source *source = (ta_source *)calloc(nf + 1, sizeof *source);
    char **owned = (char **)calloc(2 * (nf + 1), sizeof(char *));
    if (!manifest) {
        napi_throw_type_error(environment, NULL,
                              "compileSystem(manifest: string, files?: {name, xml}[])");
        goto out;
    }
    if (nf && !read_sources(environment, argv[1], nf, source + 1, owned)) goto out;
    source[0].name = (ta_string_view){"system.xml", 10};
    source[0].text = (ta_string_view){manifest, (uint32_t)ml};
    {
        nd_files files = {source + 1, nf};
        ta_compile_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.resolve = nd_resolve;
        options.resolve_context = &files;
        ta_compile_result *compiled = NULL;
        if (ta_compile_system(&options, source, &compiled) != TA_OK) {
            char message[4096] = "system compilation failed:";
            const ta_diagnostic *diagnostic;
            uint32_t nd = ta_compile_diagnostics(compiled, &diagnostic);
            for (uint32_t i = 0; i < nd && strlen(message) < sizeof message - 200; i++)
                snprintf(message + strlen(message), sizeof message - strlen(message),
                         "\n  %.*s:%u:%u: %.*s", (int)diagnostic[i].file.length,
                         diagnostic[i].file.pointer ? diagnostic[i].file.pointer : "",
                         diagnostic[i].line, diagnostic[i].column,
                         (int)diagnostic[i].message.length, diagnostic[i].message.pointer);
            napi_throw_error(environment, NULL, message);
        } else {
            ta_string_view bytes = ta_compile_image(compiled);
            result = image_value(environment, bytes.pointer, bytes.length);
        }
        ta_compile_free(compiled);
    }
out:
    for (uint32_t i = 0; i < 2 * nf; i++)
        free(owned[i]);
    free(owned);
    free(source);
    free(manifest);
    return result;
}

/* openImage(bytes) -> image: a compiled image (e.g. from the tac compiler), copied */
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

/* imageInfo(image) -> { charts: [{ name, src, datamodel }], bootActors: [{ name, chart }] } */
static napi_value js_image_info(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], o, charts;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_image *image = argc ? get_image(environment, argv[0]) : NULL;
    if (!image) return NULL;
    ta_image_info image_info;
    image_info.size = sizeof image_info;
    ta_image_get_info(&image->image, &image_info);
    napi_create_object(environment, &o);
    napi_create_array_with_length(environment, image_info.chart_count, &charts);
    for (uint32_t c = 0; c < image_info.chart_count; c++) {
        napi_value x;
        napi_create_object(environment, &x);
        napi_set_named_property(environment, x, "name",
                                str_value(environment, ta_image_chart_name(&image->image, c)));
        napi_set_named_property(environment, x, "src",
                                str_value(environment, ta_image_chart_source(&image->image, c)));
        napi_set_named_property(environment, x, "datamodel",
                                str_value(environment, ta_image_chart_datamodel(&image->image, c)));
        napi_set_element(environment, charts, c, x);
    }
    napi_set_named_property(environment, o, "charts", charts);
    napi_value boot;
    napi_create_array_with_length(environment, image_info.boot_actor_count, &boot);
    for (uint32_t i = 0; i < image_info.boot_actor_count; i++) {
        ta_string_view name;
        uint32_t chart = 0;
        napi_value x;
        napi_create_object(environment, &x);
        if (ta_image_boot_actor(&image->image, i, &name, &chart) == TA_OK) {
            napi_set_named_property(environment, x, "name", str_value(environment, name));
            napi_set_named_property(environment, x, "chart", num(environment, chart));
        }
        napi_set_element(environment, boot, i, x);
    }
    napi_set_named_property(environment, o, "bootActors", boot);
    return o;
}

static napi_value js_boot(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    return bool_value(environment,
                      ta_host_boot(host->host, get_u32(environment, argv[1])) == TA_OK);
}

static napi_value js_actor_by_name(napi_env environment, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc >= 2 ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t nl = 0;
    char *name = get_string(environment, argv[1], &nl);
    uint64_t s = name ? ta_host_actor_by_name(host->host, (ta_string_view){name, (uint32_t)nl}) : 0;
    free(name);
    return num(environment, (double)s);
}

/* actors(host) -> every session */
static napi_value js_actors(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], arr;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    napi_create_array(environment, &arr);
    uint64_t cursor = 0, chunk[256];
    uint32_t k, n = 0;
    while ((k = ta_host_actors(host->host, &cursor, chunk, 256)) > 0)
        for (uint32_t i = 0; i < k; i++)
            napi_set_element(environment, arr, n++, num(environment, (double)chunk[i]));
    return arr;
}

/* ---- snapshot / restore ------------------------------------------------------------- */

static int32_t segment_of(nd_host *host, const ta_image *image) {
    for (uint32_t i = 0; i < host->image_count; i++) {
        napi_value v = host->images[i] ? get_ref(host->environment, host->images[i]) : NULL;
        void *p = NULL;
        if (v && napi_get_value_external(host->environment, v, &p) == napi_ok && p &&
            &((nd_image *)p)->image == image)
            return (int32_t)i;
    }
    return -1;
}

/* snapshot(host) -> { host: Uint8Array, dm }: the regions, and the datamodel
 * tables they refer to by handle (plain values: the caller encodes them) */
static napi_value js_snapshot(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], out, dm, instances, pays;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    nd_host *host = argc ? get_host(environment, argv[0]) : NULL;
    if (!host) return NULL;
    size_t length = 0;
    ta_status status = ta_host_snapshot(host->host, NULL, 0, &length);
    void *buffer = status == TA_OK ? malloc(length ? length : 1) : NULL;
    if (buffer) status = ta_host_snapshot(host->host, buffer, length, &length);
    if (!buffer || status != TA_OK) {
        free(buffer);
        napi_throw_error(environment, NULL,
                         status == TA_ERROR_STATE ? "the system is busy (a migration is running)"
                                                  : "cannot snapshot the system");
        return NULL;
    }
    napi_create_object(environment, &out);
    napi_set_named_property(environment, out, "host", bytes_value(environment, buffer, length));
    free(buffer);
    napi_create_object(environment, &dm);
    napi_create_array(environment, &instances);
    uint32_t k = 0;
    for (uint32_t i = 0; i < host->instance_count; i++) {
        nd_instance *instance = &host->instances[i];
        if (!instance->live) continue;
        napi_value o;
        napi_create_object(environment, &o);
        napi_set_named_property(environment, o, "handle",
                                num(environment, (double)instance->datamodel_handle));
        napi_set_named_property(environment, o, "serial",
                                num(environment, (double)instance->serial));
        napi_set_named_property(environment, o, "seg",
                                num(environment, segment_of(host, instance->chart->image)));
        napi_set_named_property(environment, o, "chart", num(environment, instance->chart->chart));
        if (instance->object) {
            napi_value self = get_ref(environment, instance->object), state, sys;
            set_ref(environment, &host->store_error, NULL);
            state = ecmascript_state(host, instance);
            if (!state) { /* what cannot be stored, named */
                napi_value exc = host->store_error ? get_ref(environment, host->store_error) : NULL;
                set_ref(environment, &host->store_error, NULL);
                if (exc)
                    napi_throw(environment, exc);
                else
                    napi_throw_error(environment, NULL, "cannot snapshot a session");
                return NULL;
            }
            napi_get_named_property(environment, self, "sys", &sys);
            napi_set_named_property(environment, o, "es", bool_value(environment, 1));
            napi_set_named_property(environment, o, "state", state ? state : undef(environment));
            napi_set_named_property(environment, o, "sys", sys);
        } else {
            napi_set_named_property(environment, o, "context",
                                    get_ref(environment, instance->context));
            napi_set_named_property(environment, o, "event", get_ref(environment, instance->event));
            napi_set_named_property(environment, o, "init", get_ref(environment, instance->init));
            napi_set_named_property(environment, o, "doneConfig",
                                    get_ref(environment, instance->done_config));
        }
        napi_set_element(environment, instances, k++, o);
    }
    napi_create_array(environment, &pays);
    k = 0;
    for (uint32_t i = 0; i < host->payload_count; i++) {
        if (!host->refs[i]) continue;
        napi_value o;
        napi_create_object(environment, &o);
        napi_set_named_property(environment, o, "handle", num(environment, i + 1));
        napi_set_named_property(environment, o, "refs", num(environment, host->refs[i]));
        napi_set_named_property(environment, o, "value", get_ref(environment, host->payloads[i]));
        napi_set_element(environment, pays, k++, o);
    }
    napi_set_named_property(environment, dm, "insts", instances);
    napi_set_named_property(environment, dm, "pays", pays);
    napi_set_named_property(environment, out, "dm", dm);
    return out;
}

static uint32_t prop_u32(napi_env environment, napi_value o, const char *name) {
    napi_value v;
    double d = 0;
    if (napi_get_named_property(environment, o, name, &v) == napi_ok)
        napi_get_value_double(environment, v, &d);
    return d > 0 ? (uint32_t)d : 0;
}

static napi_value prop(napi_env environment, napi_value o, const char *name) {
    napi_value v = NULL;
    napi_get_named_property(environment, o, name, &v);
    return v;
}

/* restore(resolver, codec, base, es, images, host, dm) -> host: images[i] for
 * segment i (null if retired); dm as returned by snapshot() */
static napi_value js_restore(napi_env environment, napi_callback_info info) {
    size_t argc = 7;
    napi_value argv[7], result;
    napi_get_cb_info(environment, info, &argc, argv, NULL, NULL);
    void *snap = NULL;
    size_t snap_length = 0;
    if (argc < 7 || !bytes_of(environment, argv[5], &snap, &snap_length)) {
        napi_throw_type_error(environment, NULL,
                              "restore(resolver, codec, base, es, images, host, dm)");
        return NULL;
    }
    uint32_t n = 0;
    napi_get_array_length(environment, argv[4], &n);
    const ta_image **images = (const ta_image **)calloc(n + 1, sizeof *images);
    for (uint32_t i = 0; i < n; i++) {
        napi_value v;
        void *p = NULL;
        napi_get_element(environment, argv[4], i, &v);
        if (napi_get_value_external(environment, v, &p) == napi_ok && p)
            images[i] = &((nd_image *)p)->image;
    }
    ta_host_config config;
    ta_datamodel_binding binding[2];
    nd_host *host = host_alloc(environment, argv, &config, binding);
    void *copy = malloc(snap_length + 8); /* 8-aligned */
    ta_status status = TA_ERROR_NO_MEMORY;
    if (host && copy) {
        memcpy(copy, snap, snap_length);
        status = ta_host_restore(&config, images, n, copy, snap_length, &host->host);
    }
    free(copy);
    if (status != TA_OK) {
        free(images);
        if (host) host_finalize(environment, host, NULL);
        napi_throw_error(environment, NULL,
                         "cannot restore the snapshot (other images or another build?)");
        return NULL;
    }
    host->images = (napi_ref *)calloc(n + 1, sizeof(napi_ref));
    host->image_capacity = n + 1;
    host->image_count = n;
    for (uint32_t i = 0; i < n; i++)
        if (images[i]) {
            napi_value v;
            napi_get_element(environment, argv[4], i, &v);
            host->images[i] = make_ref(environment, v);
        }
    /* payloads at their handles */
    napi_value datamodel = argv[6], pays = prop(environment, datamodel, "pays"),
               instances = prop(environment, datamodel, "insts");
    uint32_t np = 0, ni = 0;
    napi_get_array_length(environment, pays, &np);
    for (uint32_t k = 0; k < np; k++) {
        napi_value e;
        napi_get_element(environment, pays, k, &e);
        uint32_t handle = prop_u32(environment, e, "handle"),
                 refs = prop_u32(environment, e, "refs");
        if (!handle || !refs) continue;
        while (host->payload_count < handle) {
            if (host->payload_count == host->payload_capacity) {
                uint32_t nc = host->payload_capacity ? host->payload_capacity * 2 : 64;
                host->payloads = (napi_ref *)realloc(host->payloads, sizeof(napi_ref) * nc);
                host->refs = (uint32_t *)realloc(host->refs, sizeof(uint32_t) * nc);
                host->payload_capacity = nc;
            }
            host->payloads[host->payload_count] = NULL;
            host->refs[host->payload_count++] = 0;
        }
        host->payloads[handle - 1] = make_ref(environment, prop(environment, e, "value"));
        host->refs[handle - 1] = refs;
        host->live_payloads++;
    }
    host->free_payload = 0;
    for (uint32_t i = host->payload_count; i-- > 0;)
        if (!host->refs[i]) {
            host->payloads[i] = (napi_ref)(uintptr_t)host->free_payload;
            host->free_payload = i + 1;
        }
    /* Reserve the slots addressed directly by system regions. */
    napi_get_array_length(environment, instances, &ni);
    uint32_t reserved = 0;
    for (uint32_t k = 0; k < ni; k++) {
        napi_value e;
        napi_get_element(environment, instances, k, &e);
        uint64_t id = get_session(environment, prop(environment, e, "handle"));
        uint32_t slot = (uint32_t)(id >= ECMASCRIPT_HANDLE ? id - ECMASCRIPT_HANDLE : id);
        if (slot > reserved) reserved = slot;
    }
    host->instance_capacity = reserved;
    host->instances = (nd_instance *)calloc(host->instance_capacity ? host->instance_capacity : 1,
                                            sizeof(nd_instance));
    host->instance_count = reserved;
    for (uint32_t k = 0; k < ni; k++) {
        napi_value e;
        napi_get_element(environment, instances, k, &e);
        uint64_t datamodel_handle = get_session(environment, prop(environment, e, "handle"));
        bool is_ecmascript = false;
        napi_value esv = prop(environment, e, "es");
        napi_valuetype t = napi_undefined;
        if (esv) napi_typeof(environment, esv, &t);
        if (t == napi_boolean) napi_get_value_bool(environment, esv, &is_ecmascript);
        uint32_t handle =
            (uint32_t)(datamodel_handle >= ECMASCRIPT_HANDLE ? datamodel_handle - ECMASCRIPT_HANDLE
                                                             : datamodel_handle);
        uint32_t segment = prop_u32(environment, e, "seg"),
                 chart = prop_u32(environment, e, "chart");
        nd_chart *binding_chart = NULL;
        for (uint32_t i = 0; i < host->chart_count && !binding_chart && segment < n; i++)
            if (host->charts[i]->image == images[segment] && host->charts[i]->chart == chart)
                binding_chart = host->charts[i];
        if (!handle || !binding_chart) continue;
        nd_instance *instance = &host->instances[handle - 1];
        memset(instance, 0, sizeof *instance);
        instance->datamodel_handle = datamodel_handle;
        instance->chart = binding_chart;
        instance->live = 1;
        {
            double d = 0;
            napi_get_value_double(environment, prop(environment, e, "serial"), &d);
            instance->serial = (uint64_t)d; /* sessions exceed 32 bits */
        }
        host->session_count++;
        napi_value f;
        napi_create_function(environment, "In", NAPI_AUTO_LENGTH, js_in, host, &f);
        set_ref(environment, &instance->in_function, f);
        map_put(host, instance->serial, handle);
        if (is_ecmascript) {
            napi_value args[4] = {get_ref(environment, binding_chart->ecmascript),
                                  prop(environment, e, "sys"), prop(environment, e, "state"), f};
            napi_value r = ecmascript_call(host, instance, "deserialize", 4, args);
            if (r) set_ref(environment, &instance->object, r);
        } else {
            set_ref(environment, &instance->context, prop(environment, e, "context"));
            set_ref(environment, &instance->event, prop(environment, e, "event"));
            set_ref(environment, &instance->init, prop(environment, e, "init"));
            napi_value dc = prop(environment, e, "doneConfig");
            napi_typeof(environment, dc, &t);
            if (t != napi_undefined && t != napi_null)
                set_ref(environment, &instance->done_config, dc);
        }
    }
    host->free_instance = 0;
    for (uint32_t i = host->instance_count; i-- > 0;)
        if (!host->instances[i].live) {
            host->instances[i].next_free = host->free_instance;
            host->free_instance = i + 1;
        }
    free(images);
    napi_create_external(environment, host, host_finalize, NULL, &result);
    return result;
}

napi_value ta_js_init(napi_env environment, napi_value exports) {
    napi_property_descriptor props[] = {
        {"compile", NULL, js_compile, NULL, NULL, NULL, napi_default_method, NULL},
        {"chartIndex", NULL, js_chart_index, NULL, NULL, NULL, napi_default_method, NULL},
        {"states", NULL, js_states, NULL, NULL, NULL, napi_default_method, NULL},
        {"compileSystem", NULL, js_compile_system, NULL, NULL, NULL, napi_default_method, NULL},
        {"openImage", NULL, js_open_image, NULL, NULL, NULL, napi_default_method, NULL},
        {"imageBytes", NULL, js_image_bytes, NULL, NULL, NULL, napi_default_method, NULL},
        {"imageInfo", NULL, js_image_info, NULL, NULL, NULL, napi_default_method, NULL},
        {"boot", NULL, js_boot, NULL, NULL, NULL, napi_default_method, NULL},
        {"actorByName", NULL, js_actor_by_name, NULL, NULL, NULL, napi_default_method, NULL},
        {"actors", NULL, js_actors, NULL, NULL, NULL, napi_default_method, NULL},
        {"snapshot", NULL, js_snapshot, NULL, NULL, NULL, napi_default_method, NULL},
        {"restore", NULL, js_restore, NULL, NULL, NULL, napi_default_method, NULL},
        {"createHost", NULL, js_create_host, NULL, NULL, NULL, napi_default_method, NULL},
        {"addImage", NULL, js_add_image, NULL, NULL, NULL, napi_default_method, NULL},
        {"chart", NULL, js_chart, NULL, NULL, NULL, napi_default_method, NULL},
        {"spawn", NULL, js_spawn, NULL, NULL, NULL, napi_default_method, NULL},
        {"send", NULL, js_send, NULL, NULL, NULL, napi_default_method, NULL},
        {"sendMany", NULL, js_send_many, NULL, NULL, NULL, napi_default_method, NULL},
        {"peerAdd", NULL, js_peer_add, NULL, NULL, NULL, napi_default_method, NULL},
        {"peerUp", NULL, js_peer_up, NULL, NULL, NULL, napi_default_method, NULL},
        {"peerRemove", NULL, js_peer_remove, NULL, NULL, NULL, napi_default_method, NULL},
        {"peerReach", NULL, js_peer_reach, NULL, NULL, NULL, napi_default_method, NULL},
        {"route", NULL, js_route, NULL, NULL, NULL, napi_default_method, NULL},
        {"exportName", NULL, js_export, NULL, NULL, NULL, napi_default_method, NULL},
        {"deliverEnvelope", NULL, js_deliver_envelope, NULL, NULL, NULL, napi_default_method, NULL},
        {"peerError", NULL, js_peer_error, NULL, NULL, NULL, napi_default_method, NULL},
        {"peerRtt", NULL, js_peer_round_trip_time, NULL, NULL, NULL, napi_default_method, NULL},
        {"peerStatus", NULL, js_peer_status, NULL, NULL, NULL, napi_default_method, NULL},
        {"run", NULL, js_run, NULL, NULL, NULL, napi_default_method, NULL},
        {"advance", NULL, js_advance, NULL, NULL, NULL, napi_default_method, NULL},
        {"config", NULL, js_config, NULL, NULL, NULL, napi_default_method, NULL},
        {"context", NULL, js_context, NULL, NULL, NULL, napi_default_method, NULL},
        {"terminate", NULL, js_terminate, NULL, NULL, NULL, napi_default_method, NULL},
        {"stats", NULL, js_stats, NULL, NULL, NULL, napi_default_method, NULL},
        {"destroy", NULL, js_destroy, NULL, NULL, NULL, napi_default_method, NULL},
        {"sweep", NULL, js_sweep, NULL, NULL, NULL, napi_default_method, NULL},
        {"hibernate", NULL, js_hibernate, NULL, NULL, NULL, napi_default_method, NULL},
        {"wake", NULL, js_wake, NULL, NULL, NULL, napi_default_method, NULL},
        {"storeError", NULL, js_store_error, NULL, NULL, NULL, napi_default_method, NULL},
        {"exportActor", NULL, js_export_actor, NULL, NULL, NULL, napi_default_method, NULL},
        {"adopt", NULL, js_adopt, NULL, NULL, NULL, napi_default_method, NULL},
        {"migrate", NULL, js_migrate, NULL, NULL, NULL, napi_default_method, NULL},
        {"resume", NULL, js_resume, NULL, NULL, NULL, napi_default_method, NULL},
    };
    napi_define_properties(environment, exports, sizeof props / sizeof props[0], props);
    return exports;
}
