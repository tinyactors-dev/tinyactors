/* module.c — the module's entry point: the environment's state and the
 * package's exports. NAPI_MODULE_INIT() defines Node-API's entry points
 * (napi_register_module_v1, node_api_module_get_api_version_v1: Node.js's
 * include/node/node_api.h); everything else here is ours. */
#include "addon.h"

#include <stdlib.h>

ta_napi_module *ta_napi_module_of(napi_env env) {
    void *data = NULL;
    napi_get_instance_data(env, &data);
    return (ta_napi_module *)data;
}

/* The environment ends: its state goes. Its references die with it. */
static void ta_napi_module_free(node_api_basic_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    free(data);
}

NAPI_MODULE_INIT() {
    ta_napi_module *module = (ta_napi_module *)calloc(1, sizeof *module);
    if (!module) {
        napi_throw_error(env, NULL, "out of memory");
        return NULL;
    }
    if (napi_set_instance_data(env, module, ta_napi_module_free, NULL) != napi_ok) {
        free(module);
        return NULL;
    }
    return exports;
}
