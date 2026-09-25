#include "napi_loads.h"

#include <stdio.h>
#include <string.h>

#include "loads.h"
#include "napi_trace.h"
#include "pump.h"
#include "runtime.h"

static napi_value tna_undefined_value(napi_env env) {
    napi_value out = NULL;
    napi_get_undefined(env, &out);
    return out;
}

static napi_status tna_put_text(napi_env env, napi_value object, const char *name, ta_str text) {
    napi_value value = NULL;
    napi_status status = napi_create_string_utf8(env, text.ptr, text.len, &value);
    return status == napi_ok ? napi_set_named_property(env, object, name, value) : status;
}

static napi_status tna_put_number(napi_env env, napi_value object, const char *name, double number) {
    napi_value value = NULL;
    napi_status status = napi_create_double(env, number, &value);
    return status == napi_ok ? napi_set_named_property(env, object, name, value) : status;
}

void tna_loads_dispatch(napi_env env, tna_napi_system *wrapper, napi_value *first_error) {
    tna_system *system = wrapper->system;
    if (tna_loads_pending_requests(&system->loads) == 0) {
        return;
    }
    napi_value requests = NULL;
    if (napi_create_array(env, &requests) != napi_ok) {
        return;
    }
    uint32_t count = 0;
    tna_load_request request;
    while (tna_loads_next_request(&system->loads, &request)) {
        ta_actor_address address = ta_address_view(request.address);
        napi_value item = NULL;
        napi_value why = NULL;
        if (napi_create_object(env, &item) != napi_ok || tna_put_number(env, item, "index", request.index) != napi_ok ||
            tna_put_number(env, item, "generation", request.generation) != napi_ok ||
            tna_put_text(env, item, "family", address.family.name) != napi_ok ||
            tna_put_text(env, item, "name", address.name) != napi_ok ||
            napi_create_string_utf8(env, request.why == TNA_LOAD_WHY_TIMER ? "timer" : "mail", NAPI_AUTO_LENGTH, &why) !=
                napi_ok ||
            napi_set_named_property(env, item, "why", why) != napi_ok ||
            napi_set_element(env, requests, count, item) != napi_ok) {
            continue;
        }
        count += 1u;
    }
    /* The environment's effect handler, with the System from its weak
     * reference: holding a function that closes over the System would keep
     * it from ever being finalized. */
    tna_env *data = wrapper->env_data;
    napi_value handler = NULL;
    napi_value argv[3] = { NULL, NULL, requests };
    if (count == 0 || data->effects_handler == NULL || wrapper->facade == NULL ||
        napi_get_reference_value(env, data->effects_handler, &handler) != napi_ok || handler == NULL ||
        napi_get_reference_value(env, wrapper->facade, &argv[0]) != napi_ok || argv[0] == NULL ||
        napi_create_string_utf8(env, "loads", NAPI_AUTO_LENGTH, &argv[1]) != napi_ok) {
        return;
    }
    napi_value global = NULL;
    napi_value ignored = NULL;
    napi_get_global(env, &global);
    if (napi_call_function(env, global, handler, 3, argv, &ignored) != napi_ok) {
        napi_value error = NULL;
        bool pending = false;
        if (napi_is_exception_pending(env, &pending) == napi_ok && pending &&
            napi_get_and_clear_last_exception(env, &error) == napi_ok && *first_error == NULL) {
            *first_error = error;
        }
    }
}

napi_value tna_set_loader_family_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_str family = { 0 };
    uint32_t max_waiting = 0;
    if (argc < 2 || !tna_text_argument(env, wrapper->system, argv[0], &family) || family.len == 0 ||
        napi_get_value_uint32(env, argv[1], &max_waiting) != napi_ok) {
        return tna_throw(env, "invalid", "setLoader: expected a family name and maxWaiting");
    }
    ta_result result = tna_loads_set_family(&wrapper->system->loads, family, max_waiting);
    tna_scratch_reset_call(&wrapper->system->scratch);
    if (result != TA_OK) {
        return tna_throw(env, "out-of-memory", "setLoader: no memory for the family");
    }
    return tna_undefined_value(env);
}

/* A load's token: its index and generation, as JavaScript holds them. */
static bool tna_load_token(napi_env env, const napi_value *argv, uint32_t *index, uint32_t *generation) {
    return napi_get_value_uint32(env, argv[0], index) == napi_ok &&
           napi_get_value_uint32(env, argv[1], generation) == napi_ok;
}

static const struct {
    const char *name;
    tna_load_outcome outcome;
} tna_load_outcomes[] = {
    { "loaded", TNA_LOAD_LOADED },       { "spawned", TNA_LOAD_SPAWNED }, { "not-found", TNA_LOAD_NOT_FOUND },
    { "rejected", TNA_LOAD_REJECTED },   { "failed", TNA_LOAD_FAILED },   { "timed-out", TNA_LOAD_TIMED_OUT },
};

/* The WARN log of a load that failed or timed out (napi ADR 0020 section 7). */
static void tna_load_log(tna_system *system, const tna_load *load, tna_load_outcome outcome, ta_str detail) {
    ta_actor_address address = ta_address_view(load->address);
    char body[512];
    int length = snprintf(body, sizeof body, "%s %.*s/%.*s: %u message(s) dropped%s%.*s",
                          outcome == TNA_LOAD_TIMED_OUT ? "load timed out for" : "load failed for",
                          (int)address.family.name.len, address.family.name.ptr, (int)address.name.len,
                          address.name.ptr, (unsigned)load->mail.count, detail.len != 0 ? ": " : "",
                          (int)detail.len, detail.ptr);
    const char *event_name = outcome == TNA_LOAD_TIMED_OUT ? "tinyactors.load.timed_out" : "tinyactors.load.failed";
    tna_pump_host_log(system, 0,
                      (tna_otlp_host_log_params){
                          .event_name = { event_name, (uint32_t)strlen(event_name) },
                          .body = { body, length > 0 ? (uint32_t)(length < (int)sizeof body ? length : (int)sizeof body - 1)
                                                     : 0u },
                          .key = { "tinyactors.load.outcome", 23 },
                          .value = { tna_load_outcome_name(outcome), (uint32_t)strlen(tna_load_outcome_name(outcome)) },
                      });
}

napi_value tna_complete_load_method(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    uint32_t index = 0;
    uint32_t generation = 0;
    ta_str outcome_text = { 0 };
    if (argc < 3 || !tna_load_token(env, argv, &index, &generation) ||
        !tna_text_argument(env, system, argv[2], &outcome_text)) {
        return tna_throw(env, "invalid", "completeLoad: expected (index, generation, outcome)");
    }
    tna_load_outcome outcome = TNA_LOAD_OUTCOMES;
    for (size_t at = 0; at < sizeof tna_load_outcomes / sizeof tna_load_outcomes[0]; at += 1u) {
        if (strlen(tna_load_outcomes[at].name) == outcome_text.len &&
            memcmp(tna_load_outcomes[at].name, outcome_text.ptr, outcome_text.len) == 0) {
            outcome = tna_load_outcomes[at].outcome;
        }
    }
    ta_actor_id actor = 0;
    if (outcome == TNA_LOAD_LOADED || outcome == TNA_LOAD_SPAWNED) {
        uint32_t slot = 0;
        uint32_t actor_generation = 0;
        if (argc < 5 || napi_get_value_uint32(env, argv[3], &slot) != napi_ok ||
            napi_get_value_uint32(env, argv[4], &actor_generation) != napi_ok) {
            tna_scratch_reset_call(&system->scratch);
            return tna_throw(env, "invalid", "completeLoad: a loaded or spawned outcome needs the actor");
        }
        actor = ta_actor_id_make(slot, actor_generation);
    }
    ta_str detail = { 0 };
    if (argc >= 6 && !tna_text_argument(env, system, argv[5], &detail)) {
        detail = (ta_str){ 0 };
    }
    if (outcome == TNA_LOAD_OUTCOMES) {
        tna_scratch_reset_call(&system->scratch);
        return tna_throw(env, "invalid", "completeLoad: unknown outcome");
    }
    const tna_load *load = tna_loads_find(&system->loads, index, generation);
    if (load != NULL && (outcome == TNA_LOAD_FAILED || outcome == TNA_LOAD_TIMED_OUT)) {
        tna_load_log(system, load, outcome, detail);
    }
    ta_result result = tna_loads_finish(&system->loads, &(tna_load_finish){ .sched = &system->sched,
                                                                            .directory = &system->directory,
                                                                            .metrics = &system->metrics,
                                                                            .index = index,
                                                                            .generation = generation,
                                                                            .outcome = outcome,
                                                                            .actor = actor,
                                                                            .now_ns = tna_monotonic_ns(NULL),
                                                                            .wall_ns = tna_system_wall_ns() });
    tna_scratch_reset_call(&system->scratch);
    if (result == TA_ERR_NOT_FOUND) {
        return tna_throw(env, "invalid", "completeLoad: the actor is not resident at the load's address");
    }
    tna_system_want_turn(system);
    tna_pump_settle_waiters(env, wrapper);
    napi_value out = NULL;
    napi_get_boolean(env, result != TA_ERR_STALE, &out);
    return out;
}

napi_value tna_load_trace_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    uint32_t index = 0;
    uint32_t generation = 0;
    if (argc < 2 || !tna_load_token(env, argv, &index, &generation)) {
        return tna_throw(env, "invalid", "loadTrace: expected (index, generation)");
    }
    const tna_load *load = tna_loads_find(&wrapper->system->loads, index, generation);
    if (load == NULL) {
        return tna_undefined_value(env);
    }
    return tna_trace_parent_object(env, &load->trace);
}

/* The interned address of (family, name) arguments; NULL after throwing. */
static ta_address *tna_address_arguments(napi_env env, tna_napi_system *wrapper, const napi_value *argv,
                                         const char *what) {
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        tna_throw(env, "reentrant", "an address cannot be interned from inside a core call (ADR 0013)");
        return NULL;
    }
    ta_actor_address text = { 0 };
    ta_address *address = NULL;
    bool ok = tna_text_argument(env, system, argv[0], &text.family.name) &&
              tna_text_argument(env, system, argv[1], &text.name);
    ta_result result = ok ? ta_address_intern(system->core, &text, &address) : TA_ERR_INVALID;
    tna_scratch_reset_call(&system->scratch);
    if (result != TA_OK) {
        char message[96];
        snprintf(message, sizeof message, "%s: expected (family, name) with non-empty text", what);
        tna_throw(env, result == TA_ERR_INVALID ? "invalid" : tna_result_code(result), message);
        return NULL;
    }
    return address;
}

napi_value tna_wake_at_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    double time = -1;
    if (argc < 3 || napi_get_value_double(env, argv[2], &time) != napi_ok || !(time >= 0) ||
        time > 9007199254740991.0 || time != (double)(ta_time)time) {
        return tna_throw(env, "invalid", "wakeAt: the time must be a non-negative integer number of milliseconds");
    }
    ta_address *address = tna_address_arguments(env, wrapper, argv, "wakeAt");
    if (address == NULL) {
        return NULL;
    }
    ta_result result = tna_loads_wake_at(&wrapper->system->loads, address, (ta_time)time);
    ta_address_release(address);
    if (result != TA_OK) {
        return tna_throw(env, "out-of-memory", "wakeAt: no memory for the wake-up");
    }
    tna_system_arm_timer(wrapper->system);
    return tna_undefined_value(env);
}

napi_value tna_cancel_wake_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_address *address = tna_address_arguments(env, wrapper, argv, "cancelWake");
    if (address == NULL) {
        return NULL;
    }
    tna_loads_cancel_wake(&wrapper->system->loads, address);
    ta_address_release(address);
    return tna_undefined_value(env);
}
