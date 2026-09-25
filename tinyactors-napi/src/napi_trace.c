#include "napi_trace.h"

#include <string.h>

#include "adapters/trace.h"
#include "pump.h"

/* ------------------------------------------------------------------------
 * Names
 */

/* ta_trace_kind, in order: the names JavaScript sees and masks with. */
static const char *const tna_trace_kinds[] = {
    "spawned",        "loaded",   "unloaded",  "finished",   "destroyed",      "macrostepBegin",
    "macrostepEnd",   "condition", "microstep", "raised",     "dataInit",       "assign",
    "script",         "foreach",  "actionFailed", "sent",    "cancelled",      "invoked",
    "invokeCancelled", "paused",  "resumed",   "stopped",
};
_Static_assert(sizeof tna_trace_kinds / sizeof tna_trace_kinds[0] == (size_t)TA_TRACE_STOPPED + 1u,
               "one name per ta_trace_kind");

static void tna_js_put(napi_env env, napi_value object, const char *name, napi_value value) {
    if (value != NULL) {
        napi_set_named_property(env, object, name, value);
    }
}

static napi_value tna_js_string(napi_env env, const char *ptr, size_t len) {
    napi_value out = NULL;
    napi_create_string_utf8(env, ptr == NULL ? "" : ptr, ptr == NULL ? 0 : len, &out);
    return out;
}

static napi_value tna_js_number(napi_env env, double value) {
    napi_value out = NULL;
    napi_create_double(env, value, &out);
    return out;
}

static napi_value tna_js_bigint(napi_env env, uint64_t value) {
    napi_value out = NULL;
    napi_create_bigint_uint64(env, value, &out);
    return out;
}

static napi_value tna_js_key_object(napi_env env, const ta_step_key *key) {
    napi_value out = NULL;
    napi_create_object(env, &out);
    tna_js_put(env, out, "session", tna_js_bigint(env, key->session));
    tna_js_put(env, out, "macrostep", tna_js_bigint(env, key->macrostep));
    tna_js_put(env, out, "microstep", tna_js_number(env, key->microstep));
    return out;
}

napi_value tna_traceparent_value(napi_env env, const ta_trace_parent *parent) {
    char text[56];
    uint32_t needed = 0;
    napi_value out = NULL;
    if (ta_traceparent_format(&(ta_traceparent_format_params){ .parent = *parent, .buffer = text,
                                                                .capacity = sizeof text, .needed = &needed }) ==
        TA_OK) {
        napi_create_string_latin1(env, text, needed - 1u, &out);
    } else {
        napi_get_undefined(env, &out);
    }
    return out;
}

/* ------------------------------------------------------------------------
 * The encoder's host (otlp.h)
 */

static uint64_t tna_host_now(void *self) {
    (void)self;
    return tna_system_wall_ns();
}

static void tna_host_trace_id(void *self, uint8_t out[16]) {
    tna_napi_system *wrapper = self;
    ta_trace_context context = ta_system_new_trace(wrapper->system->core);
    for (int at = 0; at < 8; at += 1) {
        out[at] = (uint8_t)(context.trace_id_high >> (56 - 8 * at));
        out[8 + at] = (uint8_t)(context.trace_id_low >> (56 - 8 * at));
    }
}

/*
 * A value's JSON text, cut at the limit (`values: true`). It runs inside
 * the core call that wrote the record, as the JavaScript data model does;
 * JSON.stringify runs no chart code, and a toJSON that calls into the
 * system meets the reentrancy guard.
 */
static bool tna_host_value_text(void *self, const ta_value *value, ta_str *out) {
    tna_napi_system *wrapper = self;
    napi_env env = wrapper->env;
    napi_handle_scope scope = NULL;
    if (napi_open_handle_scope(env, &scope) != napi_ok) {
        return false;
    }
    bool ok = false;
    napi_value js = NULL;
    napi_value global = NULL;
    napi_value json = NULL;
    napi_value stringify = NULL;
    napi_value text = NULL;
    napi_valuetype type = napi_undefined;
    if (tna_value_get(env, value, &js) && napi_get_global(env, &global) == napi_ok &&
        napi_get_named_property(env, global, "JSON", &json) == napi_ok &&
        napi_get_named_property(env, json, "stringify", &stringify) == napi_ok &&
        napi_call_function(env, json, stringify, 1, &js, &text) == napi_ok && napi_typeof(env, text, &type) == napi_ok &&
        type == napi_string) {
        size_t limit = wrapper->trace_value_limit < sizeof wrapper->trace_value ? wrapper->trace_value_limit
                                                                                : sizeof wrapper->trace_value - 1u;
        size_t written = 0;
        if (napi_get_value_string_utf8(env, text, wrapper->trace_value, limit + 1u, &written) == napi_ok) {
            *out = (ta_str){ wrapper->trace_value, (uint32_t)written };
            ok = true;
        }
    }
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        napi_value ignored = NULL;
        napi_get_and_clear_last_exception(env, &ignored);
        ok = false;
    }
    napi_close_handle_scope(env, scope);
    return ok;
}

/* ------------------------------------------------------------------------
 * Dispatch: the turn's spans, as one OTLP request
 */

/* The spans that ended, as a Uint8Array holding one ExportTraceServiceRequest, or NULL. */
static napi_value tna_trace_bytes(napi_env env, tna_system *system) {
    if (!system->tracing) {
        return NULL;
    }
    ta_str bytes = tna_otlp_take(&system->otlp);
    if (bytes.len == 0) {
        return NULL;
    }
    void *data = NULL;
    napi_value buffer = NULL;
    napi_value array = NULL;
    if (napi_create_arraybuffer(env, bytes.len, &data, &buffer) != napi_ok) {
        return NULL;
    }
    memcpy(data, bytes.ptr, bytes.len);
    if (napi_create_typedarray(env, napi_uint8_array, bytes.len, buffer, 0, &array) != napi_ok) {
        return NULL;
    }
    return array;
}

void tna_trace_dispatch(napi_env env, tna_napi_system *wrapper, napi_value *first_error) {
    tna_system *system = wrapper->system;
    if (!system->tracing || system->otlp.span_count == 0) {
        return;
    }
    napi_handle_scope scope = NULL;
    napi_open_handle_scope(env, &scope);
    tna_env *data = wrapper->env_data;
    napi_value handler = NULL;
    napi_value argv[3] = { NULL, NULL, NULL };
    argv[2] = tna_trace_bytes(env, system);
    bool callable = argv[2] != NULL && data->effects_handler != NULL && wrapper->facade != NULL &&
                    napi_get_reference_value(env, data->effects_handler, &handler) == napi_ok && handler != NULL &&
                    napi_get_reference_value(env, wrapper->facade, &argv[0]) == napi_ok && argv[0] != NULL &&
                    napi_create_string_utf8(env, "trace", NAPI_AUTO_LENGTH, &argv[1]) == napi_ok;
    if (callable) {
        napi_value global = NULL;
        napi_value ignored = NULL;
        napi_get_global(env, &global);
        if (napi_call_function(env, global, handler, 3, argv, &ignored) != napi_ok) {
            napi_value error = NULL;
            bool pending = false;
            if (napi_is_exception_pending(env, &pending) == napi_ok && pending &&
                napi_get_and_clear_last_exception(env, &error) == napi_ok && *first_error == NULL) {
                *first_error = error;
            }
        }
    }
    if (scope != NULL) {
        napi_close_handle_scope(env, scope);
    }
}

/* ------------------------------------------------------------------------
 * Installing the sink
 */

/* Resource attributes from [key, value, key, value, ...]: strings. */
static bool tna_set_resource(napi_env env, tna_system *system, napi_value pairs) {
    uint32_t length = 0;
    bool is_array = false;
    if (napi_is_array(env, pairs, &is_array) != napi_ok || !is_array ||
        napi_get_array_length(env, pairs, &length) != napi_ok || length % 2u != 0 || length > 64u) {
        return false;
    }
    tna_otlp_attribute attributes[32];
    char text[4096];
    size_t used = 0;
    for (uint32_t at = 0; at < length; at += 1u) {
        napi_value item = NULL;
        size_t written = 0;
        if (napi_get_element(env, pairs, at, &item) != napi_ok ||
            napi_get_value_string_utf8(env, item, text + used, sizeof text - used, &written) != napi_ok ||
            used + written + 1u >= sizeof text) {
            return false;
        }
        ta_str value = { text + used, (uint32_t)written };
        if (at % 2u == 0) {
            attributes[at / 2u].key = value;
        } else {
            attributes[at / 2u].value = value;
        }
        used += written + 1u;
    }
    return tna_otlp_set_resource(&system->otlp, attributes, length / 2u) == TA_OK;
}

/*
 * setTraceSink(installed, kinds, unsampled, values, valueLimit, links,
 * resource, seed): installs the encoder and the sink adapter, or core's
 * null sink. Returns the bytes of the spans the previous sink left open,
 * ended now (marked incomplete), for the previous JavaScript sink, or
 * undefined.
 */
napi_value tna_set_trace_sink_method(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value argv[8] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "setTraceSink cannot be called from inside a core call (ADR 0013)");
    }
    bool installed = false;
    uint32_t kinds = 0;
    bool unsampled = false;
    bool values = false;
    uint32_t value_limit = 0;
    bool links = false;
    napi_valuetype seed_type = napi_undefined;
    if (argc < 8 || napi_get_value_bool(env, argv[0], &installed) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &kinds) != napi_ok ||
        napi_get_value_bool(env, argv[2], &unsampled) != napi_ok ||
        napi_get_value_bool(env, argv[3], &values) != napi_ok ||
        napi_get_value_uint32(env, argv[4], &value_limit) != napi_ok ||
        napi_get_value_bool(env, argv[5], &links) != napi_ok || napi_typeof(env, argv[7], &seed_type) != napi_ok) {
        return tna_throw(env, "invalid",
                         "setTraceSink: expected (installed, kinds, unsampled, values, valueLimit, links, resource, seed)");
    }
    uint64_t seed = 0;
    bool lossless = true;
    if (seed_type != napi_undefined &&
        (seed_type != napi_bigint || napi_get_value_bigint_uint64(env, argv[7], &seed, &lossless) != napi_ok ||
         !lossless)) {
        return tna_throw(env, "invalid", "setTraceSink: options.seed must be a bigint of at most 64 bits");
    }

    /* The previous sink's open spans end now, for the previous JavaScript sink. */
    napi_value remaining = NULL;
    if (system->tracing) {
        tna_otlp_end_all(&system->otlp);
        remaining = tna_trace_bytes(env, system);
        tna_otlp_destroy(&system->otlp);
        system->tracing = false;
    }
    ta_result result = ta_system_set_trace_sink(system->core, ta_null_trace_sink());
    if (result == TA_OK && installed && kinds != 0) {
        tna_otlp_init(&system->otlp,
                      &(tna_otlp_config){
                          .core = system->core,
                          .allocator = tna_allocator_for(&system->allocator, TNA_MEMORY_OTHER),
                          .host = { .self = wrapper, .now = tna_host_now, .trace_id = tna_host_trace_id,
                                    .value_text = values ? tna_host_value_text : NULL },
                          .link_parents = links,
                          .values = values,
                          .version = { TINYACTORS_NAPI_VERSION, (uint32_t)(sizeof TINYACTORS_NAPI_VERSION - 1u) },
                      });
        system->tracing = true;
        ta_system_seed_trace_ids(system->core, seed_type != napi_undefined ? seed : tna_system_os_seed(system));
        wrapper->trace_value_limit = value_limit;
        if (!tna_set_resource(env, system, argv[6])) {
            tna_otlp_destroy(&system->otlp);
            system->tracing = false;
            return tna_throw(env, "invalid", "setTraceSink: options.resource must map names to strings");
        }
        result = ta_system_set_trace_sink(system->core, tna_trace_sink_adapter(system, kinds, unsampled));
        if (result != TA_OK) {
            tna_otlp_destroy(&system->otlp);
            system->tracing = false;
        }
    }
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "setTraceSink: core refused the sink");
    }
    if (remaining == NULL) {
        napi_get_undefined(env, &remaining);
    }
    return remaining;
}

/*
 * spawnTrace(traceparent): the trace parent generated spawn takes, as the
 * object its codec reads: the parsed traceparent, a new trace when there is
 * none and tracing is on, or undefined.
 */
napi_value tna_spawn_trace_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    ta_trace_parent parent;
    if (wrapper == NULL || !tna_trace_argument(env, argc < 1 ? NULL : argv[0], &parent)) {
        return NULL;
    }
    tna_system_start_trace(wrapper->system, &parent.context);
    napi_value out = NULL;
    if (parent.context.trace_id_high == 0 && parent.context.trace_id_low == 0) {
        napi_get_undefined(env, &out);
        return out;
    }
    napi_value context = NULL;
    napi_create_object(env, &out);
    napi_create_object(env, &context);
    tna_js_put(env, context, "trace_id_high", tna_js_bigint(env, parent.context.trace_id_high));
    tna_js_put(env, context, "trace_id_low", tna_js_bigint(env, parent.context.trace_id_low));
    tna_js_put(env, context, "flags", tna_js_number(env, parent.context.flags));
    tna_js_put(env, context, "detail", tna_js_number(env, parent.context.detail));
    tna_js_put(env, out, "context", context);
    tna_js_put(env, out, "cause", tna_js_key_object(env, &parent.cause));
    return out;
}

/* ------------------------------------------------------------------------
 * Trace parents from JavaScript: traceparent text, parsed by core (ADR 0073)
 */

bool tna_trace_argument(napi_env env, napi_value value, ta_trace_parent *out) {
    *out = (ta_trace_parent){ 0 };
    if (value == NULL) {
        return true;
    }
    /* The common cases first, one Node-API call each: text, or undefined. */
    char text[128];
    size_t len = 0;
    if (napi_get_value_string_latin1(env, value, text, sizeof text, &len) != napi_ok) {
        napi_valuetype type = napi_undefined;
        if (napi_typeof(env, value, &type) == napi_ok && type == napi_undefined) {
            return true;
        }
        len = sizeof text; /* not text: refused below */
    }
    if (len >= sizeof text - 1u ||
        ta_traceparent_parse(&(ta_traceparent_parse_params){ .text = { text, (uint32_t)len }, .out = out }) !=
            TA_OK) {
        tna_throw(env, "invalid", "options.traceparent is not a W3C traceparent");
        return false;
    }
    return true;
}

napi_value tna_trace_kind_names(napi_env env) {
    napi_value out = NULL;
    size_t count = sizeof tna_trace_kinds / sizeof tna_trace_kinds[0];
    napi_create_array_with_length(env, count, &out);
    for (uint32_t index = 0; index < count; index += 1u) {
        napi_set_element(env, out, index, tna_js_string(env, tna_trace_kinds[index], strlen(tna_trace_kinds[index])));
    }
    return out;
}
