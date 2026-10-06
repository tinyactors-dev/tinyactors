#include "napi_loads.h"

#include <stdio.h>
#include <string.h>

#include "tinyactors_host.h"
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
    tah_system *system = wrapper->system;
    if (tah_loads_pending_requests(tah_system_loads(system)) == 0) {
        return;
    }
    napi_value requests = NULL;
    if (napi_create_array(env, &requests) != napi_ok) {
        return;
    }
    uint32_t count = 0;
    tah_load_request request;
    while (tah_loads_next_request(tah_system_loads(system), &request)) {
        ta_actor_address address = ta_address_view(request.address);
        napi_value item = NULL;
        napi_value why = NULL;
        if (napi_create_object(env, &item) != napi_ok || tna_put_number(env, item, "index", request.index) != napi_ok ||
            tna_put_number(env, item, "generation", request.generation) != napi_ok ||
            tna_put_text(env, item, "family", address.family.name) != napi_ok ||
            tna_put_text(env, item, "name", address.name) != napi_ok ||
            napi_create_string_utf8(env,
                                    request.why == TAH_LOAD_WHY_TIMER     ? "timer"
                                    : request.why == TAH_LOAD_WHY_SESSION ? "session"
                                                                          : "mail",
                                    NAPI_AUTO_LENGTH, &why) != napi_ok ||
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
    ta_result result = tah_loads_set_family(tah_system_loads(wrapper->system), family, max_waiting);
    tah_scratch_reset_call(tah_system_scratch(wrapper->system));
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
    tah_load_outcome outcome;
} tna_load_outcomes[] = {
    { "loaded", TAH_LOAD_LOADED },       { "spawned", TAH_LOAD_SPAWNED }, { "not-found", TAH_LOAD_NOT_FOUND },
    { "rejected", TAH_LOAD_REJECTED },   { "failed", TAH_LOAD_FAILED },   { "timed-out", TAH_LOAD_TIMED_OUT },
    { "done", TAH_LOAD_DONE },
};

/* The WARN log of a load that failed or timed out (napi ADR 0020 section 7). */
static void tna_load_log(tah_system *system, const tah_load_info *load, tah_load_outcome outcome, ta_str detail) {
    ta_actor_address address = ta_address_view(load->address);
    char body[512];
    int length = snprintf(body, sizeof body, "%s %.*s/%.*s: %u message(s) dropped%s%.*s",
                          outcome == TAH_LOAD_TIMED_OUT ? "load timed out for" : "load failed for",
                          (int)address.family.name.len, address.family.name.ptr, (int)address.name.len,
                          address.name.ptr, (unsigned)load->waiting, detail.len != 0 ? ": " : "",
                          (int)detail.len, detail.ptr);
    const char *event_name = outcome == TAH_LOAD_TIMED_OUT ? "tinyactors.load.timed_out" : "tinyactors.load.failed";
    tna_pump_host_log(system, 0,
                      (tao_host_log_params){
                          .event_name = { event_name, (uint32_t)strlen(event_name) },
                          .body = { body, length > 0 ? (uint32_t)(length < (int)sizeof body ? length : (int)sizeof body - 1)
                                                     : 0u },
                          .key = { "tinyactors.load.outcome", 23 },
                          .value = { tah_load_outcome_name(outcome), (uint32_t)strlen(tah_load_outcome_name(outcome)) },
                      });
}

static napi_value tna_complete_load_inner(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    uint32_t index = 0;
    uint32_t generation = 0;
    ta_str outcome_text = { 0 };
    if (argc < 3 || !tna_load_token(env, argv, &index, &generation) ||
        !tna_text_argument(env, system, argv[2], &outcome_text)) {
        return tna_throw(env, "invalid", "completeLoad: expected (index, generation, outcome)");
    }
    tah_load_outcome outcome = TAH_LOAD_OUTCOMES;
    for (size_t at = 0; at < sizeof tna_load_outcomes / sizeof tna_load_outcomes[0]; at += 1u) {
        if (strlen(tna_load_outcomes[at].name) == outcome_text.len &&
            memcmp(tna_load_outcomes[at].name, outcome_text.ptr, outcome_text.len) == 0) {
            outcome = tna_load_outcomes[at].outcome;
        }
    }
    ta_actor_id actor = 0;
    if (outcome == TAH_LOAD_LOADED || outcome == TAH_LOAD_SPAWNED) {
        uint32_t slot = 0;
        uint32_t actor_generation = 0;
        if (argc < 5 || napi_get_value_uint32(env, argv[3], &slot) != napi_ok ||
            napi_get_value_uint32(env, argv[4], &actor_generation) != napi_ok) {
            tah_scratch_reset_call(tah_system_scratch(system));
            return tna_throw(env, "invalid", "completeLoad: a loaded or spawned outcome needs the actor");
        }
        actor = ta_actor_id_make(slot, actor_generation);
    }
    ta_str detail = { 0 };
    if (argc >= 6 && !tna_text_argument(env, system, argv[5], &detail)) {
        detail = (ta_str){ 0 };
    }
    if (outcome == TAH_LOAD_OUTCOMES) {
        tah_scratch_reset_call(tah_system_scratch(system));
        return tna_throw(env, "invalid", "completeLoad: unknown outcome");
    }
    tah_load_info load = tah_loads_info(tah_system_loads(system), index, generation);
    if (load.address != NULL && (outcome == TAH_LOAD_FAILED || outcome == TAH_LOAD_TIMED_OUT)) {
        tna_load_log(system, &load, outcome, detail);
    }
    ta_result result = tah_system_finish_load(system, &(tah_system_load_finish){ .index = index,
                                                                                .generation = generation,
                                                                                .outcome = outcome,
                                                                                .actor = actor });
    tah_scratch_reset_call(tah_system_scratch(system));
    if (result == TA_ERR_NOT_FOUND) {
        return tna_throw(env, "invalid", "completeLoad: the actor is not resident (at the load's address)");
    }
    tah_system_want_turn(system);
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
    tah_load_info load = tah_loads_info(tah_system_loads(wrapper->system), index, generation);
    if (load.address == NULL) {
        return tna_undefined_value(env);
    }
    return tna_trace_parent_object(env, &load.trace);
}

/* The interned address of (family, name) arguments; NULL after throwing. */
static ta_address *tna_address_arguments(napi_env env, tna_napi_system *wrapper, const napi_value *argv,
                                         const char *what) {
    tah_system *system = wrapper->system;
    if (tah_system_inside_core(system)) {
        tna_throw(env, "reentrant", "an address cannot be interned from inside a core call (ADR 0013)");
        return NULL;
    }
    ta_actor_address text = { 0 };
    ta_address *address = NULL;
    bool ok = tna_text_argument(env, system, argv[0], &text.family.name) &&
              tna_text_argument(env, system, argv[1], &text.name);
    ta_result result = ok ? ta_address_intern(tah_system_core(system), &text, &address) : TA_ERR_INVALID;
    tah_scratch_reset_call(tah_system_scratch(system));
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
    ta_result result = tah_loads_wake_at(tah_system_loads(wrapper->system), address, (ta_time)time);
    ta_address_release(address);
    if (result != TA_OK) {
        return tna_throw(env, "out-of-memory", "wakeAt: no memory for the wake-up");
    }
    tah_system_arm_timer(wrapper->system);
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
    tah_loads_cancel_wake(tah_system_loads(wrapper->system), address);
    ta_address_release(address);
    return tna_undefined_value(env);
}

/* ------------------------------------------------------------------------
 * Offloaded actors (core ADR 0074): unload() writes a stable named actor
 * to bytes in the system's offload store, and a load reads it back. The
 * data model decides what each value's bytes are (napi_datamodel.c).
 */

/* setOffloadValues(serialize, deserialize, byHandle) */
napi_value tna_set_offload_values_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    bool by_handle = false;
    if (argc >= 3) {
        napi_get_value_bool(env, argv[2], &by_handle);
    }
    napi_valuetype first = napi_undefined;
    napi_valuetype second = napi_undefined;
    bool functions = argc >= 2 && napi_typeof(env, argv[0], &first) == napi_ok &&
                     napi_typeof(env, argv[1], &second) == napi_ok && first == napi_function &&
                     second == napi_function;
    if (!tna_js_datamodel_set_offload(&wrapper->dm, &(tna_offload_values){
                                                        .serialize = functions ? argv[0] : NULL,
                                                        .deserialize = functions ? argv[1] : NULL,
                                                        .by_handle = by_handle || !functions,
                                                    })) {
        return tna_throw(env, "out-of-memory", "setOffloadValues: cannot keep the functions");
    }
    return tna_undefined_value(env);
}

/* setCopyOut(copy): lib/values.js's copier for this system (napi ADR 0039). */
napi_value tna_set_copy_out_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    if (argc < 1 || !tna_js_datamodel_set_copy_out(&wrapper->dm, argv[0])) {
        return tna_throw(env, "out-of-memory", "setCopyOut: cannot keep the function");
    }
    return tna_undefined_value(env);
}

/* Takes the actor back from bytes the store could not keep, so it is not lost. */
static void tna_offload_undo(tna_napi_system *wrapper, ta_bytes bytes) {
    tah_system *system = wrapper->system;
    ta_actor_id back = 0;
    tah_system_enter_core(system);
    ta_result reloaded = ta_actor_load(tah_system_core(system), &(ta_actor_load_params){ .bytes = bytes, .out = &back });
    tah_system_leave_core(system);
    if (reloaded == TA_OK) {
        (void)tah_hook_actor_created(system, back);
    }
}

/* offload(slot, generation): the actor is unloaded, its bytes in the store. */
static napi_value tna_offload_inner(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    if (tah_system_inside_core(system)) {
        return tna_throw(env, "reentrant", "unload() cannot run inside a core call (ADR 0013)");
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (argc < 2 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "offload: expected (slot, generation)");
    }
    ta_actor_id actor = ta_actor_id_make(slot, generation);
    ta_actor_inspection view;
    ta_result result = ta_actor_inspect(tah_system_core(system), actor, &view);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "unload(): no such actor");
    }
    if (view.self.kind != TA_ACTOR_REF_NAMED) {
        return tna_throw(env, "invalid", "unload(): only a named actor can be unloaded");
    }
    /* The directory keeps the unloaded address's reference, which keys the store. */
    const ta_address *address = view.self.as.named;
    ta_value root = { 0 };
    bool has_root = ta_actor_datamodel_root(tah_system_core(system), actor, &root) == TA_OK;
    tna_js_datamodel_offload_begin(&wrapper->dm);
    ta_bytes bytes = { 0 };
    tah_system_enter_core(system);
    result = ta_actor_offload(tah_system_core(system), &(ta_actor_offload_params){ .actor = actor,
                                                                        .root = has_root ? &root : NULL,
                                                                        .arena = tah_scratch_call(tah_system_scratch(system)),
                                                                        .out = &bytes });
    tah_system_leave_core(system);
    tna_offload_handles handles = tna_js_datamodel_offload_end(&wrapper->dm);
    ta_value_release(&root);
    if (result != TA_OK) {
        tna_js_datamodel_release_handles(&wrapper->dm, handles);
        tah_scratch_reset_call(tah_system_scratch(system));
        return tna_throw(env, tna_result_code(result),
                         result == TA_ERR_BUSY ? "unload(): the actor is not stable; unload it once it is idle"
                                               : "unload(): the actor cannot be offloaded");
    }
    (void)tah_hook_actor_unloaded(system, actor);
    result = tah_offloads_put(tah_system_offloads(system), &(tah_offload_put_params){ .address = address,
                                                                            .bytes = bytes,
                                                                            .handles = handles.handles,
                                                                            .handle_count = handles.count });
    if (result != TA_OK) {
        tna_offload_undo(wrapper, bytes);
        tna_js_datamodel_release_handles(&wrapper->dm, handles);
        tah_scratch_reset_call(tah_system_scratch(system));
        return tna_throw(env, "out-of-memory", "unload(): no memory for the offloaded bytes");
    }
    tah_scratch_reset_call(tah_system_scratch(system));
    return tna_undefined_value(env);
}

/* hasOffloaded(family, name) */
napi_value tna_has_offloaded_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_address *address = tna_address_arguments(env, wrapper, argv, "hasOffloaded");
    if (address == NULL) {
        return NULL;
    }
    ta_bytes bytes;
    bool found = tah_offloads_bytes(tah_system_offloads(wrapper->system), address, &bytes);
    ta_address_release(address);
    napi_value out = NULL;
    napi_get_boolean(env, found, &out);
    return out;
}

/* dropOffloaded(family, name): the address is being removed. */
napi_value tna_drop_offloaded_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_address *address = tna_address_arguments(env, wrapper, argv, "dropOffloaded");
    if (address == NULL) {
        return NULL;
    }
    (void)tah_offloads_drop(tah_system_offloads(wrapper->system), address);
    ta_address_release(address);
    return tna_undefined_value(env);
}

/*
 * loadOffloaded(family, name, binding?): the actor comes back from its
 * bytes, which the store then frees; returns { slot, generation }. A
 * binding other than undefined replaces the data-model root the bytes
 * carry (ta_actor_load_params.datamodel_binding).
 */
static napi_value tna_load_offloaded_inner(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    ta_address *address = tna_address_arguments(env, wrapper, argv, "load");
    if (address == NULL) {
        return NULL;
    }
    ta_bytes bytes;
    if (!tah_offloads_bytes(tah_system_offloads(system), address, &bytes)) {
        ta_address_release(address);
        return tna_throw(env, "not-found", "load: the address has no offloaded actor");
    }
    ta_value binding = { NULL, NULL };
    napi_valuetype binding_type = napi_undefined;
    bool bound = argc > 2 && argv[2] != NULL && napi_typeof(env, argv[2], &binding_type) == napi_ok &&
                 binding_type != napi_undefined;
    if (bound && !tna_value_make(&wrapper->values, argv[2], &binding)) {
        ta_address_release(address);
        return tna_throw(env, "out-of-memory", "load: cannot keep the binding");
    }
    ta_actor_id actor = 0;
    tah_system_enter_core(system);
    ta_result result = ta_actor_load(tah_system_core(system), &(ta_actor_load_params){
                                                       .bytes = bytes,
                                                       .datamodel_binding = bound ? &binding : NULL,
                                                       .out = &actor });
    tah_system_leave_core(system);
    /* Core retained the binding if it kept it. */
    ta_value_release(&binding);
    if (result == TA_OK) {
        /* The actor holds its own references now; the entry's go with it. */
        (void)tah_offloads_drop(tah_system_offloads(system), address);
        result = tah_hook_actor_created(system, actor);
    }
    ta_address_release(address);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "load: the offloaded actor cannot be loaded");
    }
    napi_value out = NULL;
    napi_value slot = NULL;
    napi_value generation = NULL;
    if (napi_create_object(env, &out) != napi_ok ||
        napi_create_uint32(env, ta_actor_id_slot(actor), &slot) != napi_ok ||
        napi_create_uint32(env, ta_actor_id_generation(actor), &generation) != napi_ok ||
        napi_set_named_property(env, out, "slot", slot) != napi_ok ||
        napi_set_named_property(env, out, "generation", generation) != napi_ok) {
        return NULL;
    }
    return out;
}

/* complete_load, then a request for the sink if it filled the staging (napi ADR 0036). */
napi_value tna_complete_load_method(napi_env env, napi_callback_info info) {
    return tna_trace_after_method(env, info, tna_complete_load_inner(env, info));
}

/* offload, then a request for the sink if it filled the staging (napi ADR 0036). */
napi_value tna_offload_method(napi_env env, napi_callback_info info) {
    return tna_trace_after_method(env, info, tna_offload_inner(env, info));
}

/* load_offloaded, then a request for the sink if it filled the staging (napi ADR 0036). */
napi_value tna_load_offloaded_method(napi_env env, napi_callback_info info) {
    return tna_trace_after_method(env, info, tna_load_offloaded_inner(env, info));
}
