#include "runtime.h"

#include <stdlib.h>

#include "napi_system.h"

static void tna_env_free_if_unused(tna_env *data) {
    if (data->torn_down && data->references == 0) {
        free(data);
    }
}

/*
 * The environment is going away: close every System still open (ADR 0012)
 * and drop the error class. The record itself stays until the last System
 * wrapper is finalized, because finalizers may run after this hook.
 */
static void tna_env_cleanup(void *arg) {
    tna_env *data = arg;
    tna_napi_system_close_open(data);
    data->torn_down = true;
    /* The environment is being destroyed; its references go with it. */
    data->error_class = NULL;
    tna_env_free_if_unused(data);
}

napi_status tna_env_init(napi_env env, tna_env **out) {
    tna_env *data = calloc(1, sizeof *data);
    if (data == NULL) {
        return napi_generic_failure;
    }
    napi_status status = napi_set_instance_data(env, data, NULL, NULL);
    if (status == napi_ok) {
        status = napi_add_env_cleanup_hook(env, tna_env_cleanup, data);
    }
    if (status != napi_ok) {
        free(data);
        return status;
    }
    *out = data;
    return napi_ok;
}

tna_env *tna_env_get(napi_env env) {
    void *data = NULL;
    if (napi_get_instance_data(env, &data) != napi_ok) {
        return NULL;
    }
    return data;
}

void tna_env_retain(tna_env *data) {
    data->references += 1u;
}

void tna_env_release(tna_env *data) {
    data->references -= 1u;
    tna_env_free_if_unused(data);
}

napi_value tna_throw(napi_env env, const char *code, const char *message) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        return NULL;
    }
    tna_env *data = tna_env_get(env);
    napi_value error_class = NULL;
    if (data != NULL && data->error_class != NULL &&
        napi_get_reference_value(env, data->error_class, &error_class) == napi_ok && error_class != NULL) {
        napi_value argv[2];
        napi_value error = NULL;
        if (napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &argv[0]) == napi_ok &&
            napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &argv[1]) == napi_ok &&
            napi_new_instance(env, error_class, 2, argv, &error) == napi_ok) {
            napi_throw(env, error);
            return NULL;
        }
    }
    napi_throw_error(env, code, message);
    return NULL;
}

const char *tna_result_code(ta_result result) {
    switch (result) {
    case TA_ERR_INVALID: return "invalid";
    case TA_ERR_NOMEM: return "out-of-memory";
    case TA_ERR_NOT_FOUND: return "not-found";
    case TA_ERR_NOT_RECONSTRUCTIBLE: return "not-reconstructible";
    case TA_ERR_BUSY: return "busy";
    case TA_ERR_UNSUPPORTED: return "unsupported";
    case TA_ERR_CORRUPT: return "corrupt";
    case TA_ERR_DEFINITION_MISMATCH: return "definition-mismatch";
    case TA_ERR_UNLOADED: return "unloaded";
    case TA_ERR_STALE: return "stale";
    case TA_ERR_DONE: return "done";
    default: return "invalid";
    }
}
