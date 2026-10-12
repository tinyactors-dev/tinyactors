/* request.c — the rarely called API (bindings.md, section 10): a method's
 * call built from the caller's values with the value table, sent with
 * ta_host_request, its result returned and its error thrown as
 * TinyactorsError with code, reason, path and message unchanged. And a
 * refused config, whose error ta_host_create writes in the same shape. */
#include "addon.h"

#include <stdlib.h>
#include <string.h>

/* The reply to `request` (CBOR), grown until it fits: TA_ERROR_NO_MEMORY
 * means nothing was applied (host.h), so asking again is safe. */
static ta_status ta_napi_ask(ta_host *host, ta_bytes request, ta_buffer *reply) {
    reply->capacity = 1024;
    reply->length = 0;
    reply->pointer = malloc(reply->capacity);
    if (!reply->pointer) return TA_ERROR_NO_MEMORY;
    ta_status status = ta_host_request(host, request, reply);
    while (status == TA_ERROR_NO_MEMORY && reply->length > reply->capacity) {
        void *bigger = realloc(reply->pointer, reply->length);
        if (!bigger) break;
        reply->pointer = bigger;
        reply->capacity = reply->length;
        status = ta_host_request(host, request, reply);
    }
    return status;
}

/* A member of a reply map, or NULL. */
static napi_value ta_napi_reply_member(napi_env env, napi_value reply, const char *name) {
    bool has = false;
    napi_value value = NULL;
    if (napi_has_named_property(env, reply, name, &has) != napi_ok || !has ||
        napi_get_named_property(env, reply, name, &value) != napi_ok)
        return NULL;
    return value;
}

/* request(method, params): {"method", "params", "id"} through the value
 * table, then the result, or the error thrown. */
napi_value ta_napi_request(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_of(env, info, &call);
    if (!host) return NULL;
    /* the caller's values go as they are: core checks them and refuses
     * (bindings.md, section 2: "It decides no policy") */
    napi_valuetype type = napi_undefined;
    napi_value message, id;
    napi_create_object(env, &message);
    napi_set_named_property(env, message, "method", call.argv[0]);
    if (napi_typeof(env, call.argv[1], &type) == napi_ok && type != napi_undefined)
        napi_set_named_property(env, message, "params", call.argv[1]);
    napi_create_uint32(env, 1, &id);
    napi_set_named_property(env, message, "id", id);
    ta_buffer request;
    ta_status status = ta_napi_values_encode(env, message, &request);
    if (status != TA_OK) {
        ta_napi_throw_status(env, "ta_host_request", status);
        return NULL;
    }
    ta_buffer reply = {NULL, 0, 0};
    status = ta_napi_ask(host->host, (ta_bytes){request.pointer, request.length}, &reply);
    free(request.pointer);
    napi_value value = NULL;
    if (status == TA_OK) status = ta_napi_values_decode(env, (ta_bytes){reply.pointer, reply.length}, &value);
    free(reply.pointer);
    if (status != TA_OK) {
        ta_napi_throw_status(env, "ta_host_request", status);
        return NULL;
    }
    napi_value error = ta_napi_reply_member(env, value, "error");
    if (error) {
        ta_napi_throw_reply(env, error);
        return NULL;
    }
    napi_value result = ta_napi_reply_member(env, value, "result");
    if (!result) napi_get_undefined(env, &result);
    return result;
}

void ta_napi_throw_config(napi_env env, ta_bytes bytes) {
    napi_value refusal, error;
    if (ta_napi_values_decode(env, bytes, &refusal) != TA_OK || !(error = ta_napi_reply_member(env, refusal, "error"))) {
        ta_napi_throw_status(env, "ta_host_create", TA_ERROR_INVALID);
        return;
    }
    ta_napi_throw_reply(env, error);
}
