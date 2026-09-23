/*
 * Module init. It registers the addon's exports and nothing else; every other
 * module is plumbing or generated code (ADR 0013).
 *
 * Exports:
 * - version: the package version;
 * - setup(TinyactorsError): registers the JavaScript error class that native
 *   code throws (ADR 0008); lib/errors.js calls it once per environment;
 * - System: the native system class (ADR 0012), used by lib/system.js;
 * - environmentStats(): counters for tests (systems created and closed,
 *   bytes leaked at close);
 * - core: the generated control plane (ADR 0013), one function per bound
 *   core function. It is internal: index.js does not re-export it.
 */
#include <node_api.h>

#include "gen/ta_napi.h"
#include "napi_system.h"
#include "runtime.h"

#ifndef TINYACTORS_NAPI_VERSION
#define TINYACTORS_NAPI_VERSION "0.0.0"
#endif

static napi_value tna_setup(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    napi_valuetype type = napi_undefined;
    if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok || type != napi_function) {
        return tna_throw(env, "invalid", "setup expects the TinyactorsError class");
    }
    tna_env *data = tna_env_get(env);
    if (data == NULL) {
        return tna_throw(env, "invalid", "the addon is not initialized");
    }
    if (data->error_class != NULL) {
        napi_delete_reference(env, data->error_class);
        data->error_class = NULL;
    }
    if (napi_create_reference(env, argv[0], 1, &data->error_class) != napi_ok) {
        return tna_throw(env, "out-of-memory", "cannot keep the error class");
    }
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

static napi_value tna_environment_stats(napi_env env, napi_callback_info info) {
    (void)info;
    tna_env *data = tna_env_get(env);
    napi_value stats = NULL;
    napi_value value = NULL;
    if (data == NULL || napi_create_object(env, &stats) != napi_ok) {
        return NULL;
    }
    napi_create_double(env, (double)data->systems_created, &value);
    napi_set_named_property(env, stats, "systemsCreated", value);
    napi_create_double(env, (double)data->systems_closed, &value);
    napi_set_named_property(env, stats, "systemsClosed", value);
    napi_create_double(env, (double)data->leaked_bytes, &value);
    napi_set_named_property(env, stats, "leakedBytes", value);
    return stats;
}

static napi_status tna_export_function(napi_env env, napi_value exports, const char *name, napi_callback callback) {
    napi_value function = NULL;
    napi_status status = napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, NULL, &function);
    return status == napi_ok ? napi_set_named_property(env, exports, name, function) : status;
}

static napi_value tna_init(napi_env env, napi_value exports) {
    tna_env *data = NULL;
    napi_value version = NULL;
    if (tna_env_init(env, &data) != napi_ok ||
        napi_create_string_utf8(env, TINYACTORS_NAPI_VERSION, NAPI_AUTO_LENGTH, &version) != napi_ok ||
        napi_set_named_property(env, exports, "version", version) != napi_ok ||
        tna_export_function(env, exports, "setup", tna_setup) != napi_ok ||
        tna_export_function(env, exports, "environmentStats", tna_environment_stats) != napi_ok ||
        tna_napi_system_define(env, exports) != napi_ok ||
        tna_gen_define(env, exports) != napi_ok) {
        napi_throw_error(env, NULL, "tinyactors-napi: module initialization failed");
        return NULL;
    }
    return exports;
}

NAPI_MODULE(tinyactors_napi, tna_init)
