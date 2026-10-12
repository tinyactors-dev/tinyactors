/* request.c — the rarely called API (bindings.md, section 10): a method's
 * call built from the caller's values with the value table, sent with
 * ta_host_request, its result returned and its error thrown as
 * TinyactorsError with code, reason, path and message unchanged. And a
 * refused config, whose error ta_host_create writes in the same shape. */
#include "addon.h"

#include <string.h>

/* The reply to `request` (CBOR), in the host's wire buffer, grown until
 * it fits: TA_ERROR_NO_MEMORY means nothing was applied (host.h), so
 * asking again is safe. */
typedef struct ta_napi_request_call {
    ta_host *host;
    ta_bytes request;
} ta_napi_request_call;

static ta_status ta_napi_request_write(void *context, ta_buffer *out) {
    const ta_napi_request_call *call = (const ta_napi_request_call *)context;
    return ta_host_request(call->host, call->request, out);
}

static ta_status ta_napi_ask(ta_napi_host *host, ta_bytes request, ta_bytes *reply) {
    ta_napi_request_call call = {host->host, request};
    ta_napi_writing writing = {ta_napi_request_write, &call};
    return ta_napi_wire(&host->scratch, &writing, reply);
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
    ta_bytes request;
    ta_status status = ta_napi_values_encode(&host->scratch, message, &request);
    if (status != TA_OK) {
        ta_napi_throw_status(env, "ta_host_request", status);
        return NULL;
    }
    ta_bytes reply;
    status = ta_napi_ask(host, request, &reply);
    if (!host->running) ta_napi_datamodel_drain(&host->native); /* a request can release payloads */
    napi_value value = NULL;
    if (status == TA_OK) status = ta_napi_values_decode(&host->scratch, reply, &value);
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

/* host.config's system.ticksPerSecond: the rate the host runs on, every
 * default filled in by core (requests.md, "host.config"), read once after
 * the host is created; never a default of the binding's own. */
ta_status ta_napi_host_read_ticks(ta_napi_host *host) {
    napi_env env = host->env;
    static const unsigned char request[] = {0xa2, 0x62, 'i', 'd', 0x01, 0x66, 'm', 'e', 't', 'h', 'o', 'd',
                                            0x6b, 'h',  'o', 's', 't', '.',  'c', 'o', 'n', 'f', 'i', 'g'};
    ta_bytes reply;
    ta_status status = ta_napi_ask(host, (ta_bytes){request, sizeof request}, &reply);
    napi_value value = NULL, result, system, ticks;
    if (status == TA_OK) status = ta_napi_values_decode(&host->scratch, reply, &value);
    double rate = 0;
    if (status != TA_OK || !(result = ta_napi_reply_member(env, value, "result")) ||
        !(system = ta_napi_reply_member(env, result, "system")) ||
        !(ticks = ta_napi_reply_member(env, system, "ticksPerSecond")) ||
        napi_get_value_double(env, ticks, &rate) != napi_ok || !(rate >= 1))
        return status == TA_OK ? TA_ERROR_INVALID : status;
    host->ticks_per_second = (uint64_t)rate;
    return TA_OK;
}

void ta_napi_throw_config(napi_env env, ta_bytes bytes) {
    napi_value refusal, error;
    if (ta_napi_values_decode(&ta_napi_module_of(env)->scratch, bytes, &refusal) != TA_OK ||
        !(error = ta_napi_reply_member(env, refusal, "error"))) {
        ta_napi_throw_status(env, "ta_host_create", TA_ERROR_INVALID);
        return;
    }
    ta_napi_throw_reply(env, error);
}
