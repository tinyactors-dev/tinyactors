/* wasm transport for the synchronous Node-API subset. No binding policy. */
#include "node_api.h"
#include <stdlib.h>

#define EXPORT(name) __attribute__((export_name(#name)))
extern napi_value ta_js_init(napi_env environment, napi_value exports);

EXPORT(w_init) napi_value w_init(napi_value exports) {
    return ta_js_init(NULL, exports);
}

EXPORT(w_call) napi_value w_call(napi_callback callback, napi_callback_info info) {
    return callback(NULL, info);
}

EXPORT(w_finalize) void w_finalize(napi_finalize callback, void *data, void *hint) {
    callback(NULL, data, hint);
}

EXPORT(w_alloc) void *w_alloc(size_t n) {
    return malloc(n ? n : 1);
}

EXPORT(w_free) void w_free(void *p) {
    free(p);
}
