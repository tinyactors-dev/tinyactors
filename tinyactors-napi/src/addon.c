#include <node_api.h>

#ifndef TINYACTORS_NAPI_VERSION
#define TINYACTORS_NAPI_VERSION "0.0.0"
#endif

static napi_value tinyactors_napi_init(napi_env env, napi_value exports) {
    napi_value version;
    if (napi_create_string_utf8(env, TINYACTORS_NAPI_VERSION, NAPI_AUTO_LENGTH, &version) != napi_ok) {
        return NULL;
    }
    if (napi_set_named_property(env, exports, "version", version) != napi_ok) {
        return NULL;
    }
    return exports;
}

NAPI_MODULE(tinyactors_napi, tinyactors_napi_init)
