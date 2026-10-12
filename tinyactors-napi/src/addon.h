/* addon.h — what the addon's files share: core's headers, Node-API's, and
 * the state of one environment (the main thread, or a worker). Node-API
 * values mean nothing outside the environment that made them, so every
 * piece of state lives in the environment's instance data
 * (napi_set_instance_data) or on a Host, never in a global (AGENTS.md,
 * "C rules"). */
#ifndef TA_NAPI_ADDON_H
#define TA_NAPI_ADDON_H

#include <node_api.h>

#include "tinyactors.h"
#include "tinyactors_datamodel_ecmascript.h"
#include "tinyactors_runtime.h"

/* One environment's state: made at module init, freed with the
 * environment. */
typedef struct ta_napi_module {
    napi_ref error_class; /* TinyactorsError */
} ta_napi_module;

/* The environment's state. */
ta_napi_module *ta_napi_module_of(napi_env env);

#endif /* TA_NAPI_ADDON_H */
