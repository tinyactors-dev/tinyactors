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

/* ---- errors (module.c) ---------------------------------------------------------- */

/* Throw a TinyactorsError for a status core answered (`status`: the
 * ta_status; the message is its name in types.h, TA_ERROR_INVALID ...),
 * unless an exception is pending already: then that one propagates. */
void ta_napi_throw_status(napi_env env, ta_status status);
/* Throw a TinyactorsError for the `error` member of a refused request or
 * config ({"code", "message", "data": {"reason", "path"}}, requests.md):
 * its message, code, reason and path, unchanged. */
void ta_napi_throw_reply(napi_env env, napi_value error);

/* ---- values (values.c): the value table of docs/values.md ------------------------ */

/* `value` as CBOR in out->pointer (malloc'd; the caller frees it). */
ta_status ta_napi_values_encode(napi_env env, napi_value value, ta_buffer *out);
/* The value of `cbor`; TA_ERROR_INVALID when it is not a neutral value. */
ta_status ta_napi_values_decode(napi_env env, ta_bytes cbor, napi_value *out);
/* A copy of the bytes of an ArrayBuffer, a typed array or a DataView
 * (malloc'd: free out->pointer); 0 when `value` is none of them. */
int ta_napi_values_bytes(napi_env env, napi_value value, ta_bytes *out);
/* The exports encode(value) and decode(bytes). */
napi_value ta_napi_encode(napi_env env, napi_callback_info info);
napi_value ta_napi_decode(napi_env env, napi_callback_info info);

#endif /* TA_NAPI_ADDON_H */
