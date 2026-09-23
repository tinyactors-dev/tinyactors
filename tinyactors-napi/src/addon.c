/*
 * Module init (ROADMAP step 1). It registers the addon's exports and nothing
 * else; every other module is plumbing or generated code (ADR 0013).
 */
#include <node_api.h>

#ifndef TINYACTORS_NAPI_VERSION
#define TINYACTORS_NAPI_VERSION "0.0.0"
#endif

static napi_value tna_init(napi_env env, napi_value exports) {
    napi_value version = NULL;
    if (napi_create_string_utf8(env, TINYACTORS_NAPI_VERSION, NAPI_AUTO_LENGTH, &version) != napi_ok ||
        napi_set_named_property(env, exports, "version", version) != napi_ok) {
        return NULL;
    }
    return exports;
}

NAPI_MODULE(tinyactors_napi, tna_init)
