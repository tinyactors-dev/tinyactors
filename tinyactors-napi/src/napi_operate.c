#include "napi_operate.h"

#include "tinyactors_host.h"
#include "runtime.h"

/* The resident actor (slot, generation) named by argv[0] and argv[1]. */
static bool tna_actor_arguments(napi_env env, napi_value *argv, size_t argc, ta_actor_id *out) {
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (argc < 2 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok) {
        return false;
    }
    *out = ta_actor_id_make(slot, generation);
    return true;
}

/*
 * recorderEvicted(slot, generation, wake?): the recorded actor is about to
 * be evicted; its address's wake is `wake` (a number) or none (kernel ADR
 * 0006 section 2).
 */
napi_value tna_recorder_evicted_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_actor_id actor = 0;
    if (!tna_actor_arguments(env, argv, argc, &actor)) {
        return tna_throw(env, "invalid", "recorderEvicted: expected (slot, generation, wake?)");
    }
    double wake = 0;
    napi_valuetype type = napi_undefined;
    bool has_wake = argc >= 3 && argv[2] != NULL && napi_typeof(env, argv[2], &type) == napi_ok &&
                    type == napi_number && napi_get_value_double(env, argv[2], &wake) == napi_ok && wake >= 0;
    tah_recorder_evicted(wrapper->system, actor, has_wake, (ta_time)wake);
    return NULL;
}

/* recorderForget(slot, generation): the recorded actor is about to be destroyed: its address needs no wake. */
napi_value tna_recorder_forget_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_actor_id actor = 0;
    if (!tna_actor_arguments(env, argv, argc, &actor)) {
        return tna_throw(env, "invalid", "recorderForget: expected (slot, generation)");
    }
    tah_recorder_forget(wrapper->system, actor);
    return NULL;
}

/* withdrawSend(slot, generation, send): the operator withdraws a pending delayed send (core ADR 0084). */
napi_value tna_withdraw_send_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    if (tah_system_inside_core(system)) {
        return tna_throw(env, "reentrant", "withdrawSend: cannot be called from inside a core call (ADR 0013)");
    }
    ta_actor_id actor = 0;
    uint64_t send = 0;
    bool lossless = false;
    if (!tna_actor_arguments(env, argv, argc, &actor) || argc < 3 ||
        napi_get_value_bigint_uint64(env, argv[2], &send, &lossless) != napi_ok || !lossless) {
        return tna_throw(env, "invalid", "withdrawSend: expected (slot, generation, send ID)");
    }
    ta_result result = tah_system_withdraw_send(system, actor, send);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "withdrawSend: core refused");
    }
    return NULL;
}

/* abandon(slot, generation): the operator abandons the actor's macrostep (core ADR 0082). */
napi_value tna_abandon_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    if (tah_system_inside_core(system)) {
        return tna_throw(env, "reentrant", "abandon: cannot be called from inside a core call (ADR 0013)");
    }
    ta_actor_id actor = 0;
    if (!tna_actor_arguments(env, argv, argc, &actor)) {
        return tna_throw(env, "invalid", "abandon: expected (slot, generation)");
    }
    ta_result result = tah_system_abandon(system, actor);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "abandon: core refused");
    }
    return NULL;
}
