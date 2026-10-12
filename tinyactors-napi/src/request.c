/* request.c — the rarely called API (bindings.md, section 10): a method's
 * call built from the caller's values with the value table, sent with
 * ta_host_request, its result returned and its error thrown as
 * TinyactorsError with code, reason, path and message unchanged. And a
 * refused config, whose error ta_host_create writes in the same shape. */
#include "addon.h"

#include <string.h>

/* A member of a reply map, or NULL. */
static napi_value ta_napi_reply_member(napi_env env, napi_value reply, const char *name) {
    bool has = false;
    napi_value value = NULL;
    if (napi_has_named_property(env, reply, name, &has) != napi_ok || !has ||
        napi_get_named_property(env, reply, name, &value) != napi_ok)
        return NULL;
    return value;
}

/* request(method, params): {"method", "params", "id"} written by the value
 * table with a writer of this call's, sent with ta_host_request, its reply
 * (a view of the host's memory until the next call into it) read into
 * values before anything else runs; then the result, or the error thrown. */
napi_value ta_napi_request(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_of(env, info, &call);
    if (!host) return NULL;
    /* the caller's values go as they are: core checks them and refuses
     * (bindings.md, section 2: "It decides no policy") */
    ta_napi_encoder encoder;
    ta_napi_encoder_init(&encoder, env, NULL);
    ta_cbor_write_map(&encoder.writer, TA_CBOR_COUNT_LATER);
    ta_napi_encoder_member(&encoder, "method", call.argv[0]);
    ta_napi_encoder_member(&encoder, "params", call.argv[1]);
    ta_cbor_write_text(&encoder.writer, (ta_string_view){"id", 2});
    ta_cbor_write_uint(&encoder.writer, 1);
    ta_cbor_write_end(&encoder.writer);
    ta_bytes request, reply;
    napi_value value = NULL;
    ta_status status = ta_napi_encoder_bytes(&encoder, &request);
    if (status == TA_OK) status = ta_host_request(host->host, request, &reply);
    if (status == TA_OK) status = ta_napi_values_decode(env, reply, &value);
    ta_napi_encoder_free(&encoder);
    if (!host->running) ta_napi_datamodel_drain(&host->native); /* a request can release payloads */
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
    ta_status status = ta_host_request(host->host, (ta_bytes){request, sizeof request}, &reply);
    napi_value value = NULL, result, system, ticks;
    if (status == TA_OK) status = ta_napi_values_decode(env, reply, &value);
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
    if (ta_napi_values_decode(env, bytes, &refusal) != TA_OK ||
        !(error = ta_napi_reply_member(env, refusal, "error"))) {
        ta_napi_throw_status(env, "ta_host_create", TA_ERROR_INVALID);
        return;
    }
    ta_napi_throw_reply(env, error);
}
