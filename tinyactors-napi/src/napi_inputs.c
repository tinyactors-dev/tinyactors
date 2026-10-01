#include "napi_inputs.h"

#include <string.h>

#include "tinyactors_host.h"
#include "pump.h"
#include "runtime.h"
#include "values.h"

static void tna_put(napi_env env, napi_value object, const char *name, napi_value value) {
    if (value != NULL) {
        napi_set_named_property(env, object, name, value);
    }
}

void tna_inputs_dispatch(napi_env env, tna_napi_system *wrapper, napi_value *first_error) {
    tah_system *system = wrapper->system;
    tah_inputs *inputs = tah_system_inputs(system);
    if (!tah_inputs_enabled(inputs) && tah_inputs_entry_count(inputs) == 0 && tah_inputs_touched_count(inputs) == 0) {
        return;
    }
    napi_handle_scope scope = NULL;
    napi_open_handle_scope(env, &scope);
    uint32_t wake_count = 0;
    const tah_input_wake *wakes = tah_inputs_wakes(system, &wake_count);
    napi_value batch = NULL;
    napi_value list = NULL;
    napi_value wake_list = NULL;
    bool made = napi_create_object(env, &batch) == napi_ok && napi_create_array(env, &list) == napi_ok &&
                napi_create_array_with_length(env, wake_count, &wake_list) == napi_ok;
    uint32_t at = 0;
    uint32_t entries = tah_inputs_entry_count(inputs);
    for (uint32_t index = 0; made && index < entries; index += 1u) {
        tah_input_entry entry = tah_inputs_entry_at(inputs, index);
        if (entry.length == 0) {
            continue; /* its only record was refused */
        }
        napi_value item = NULL;
        napi_value bytes = NULL;
        void *data = NULL;
        ta_actor_address address = ta_address_view(entry.address);
        if (napi_create_object(env, &item) != napi_ok ||
            napi_create_buffer_copy(env, entry.length, entry.bytes, &data, &bytes) != napi_ok) {
            continue;
        }
        tna_put(env, item, "address", tna_address_object(env, &address));
        tna_put(env, item, "bytes", bytes);
        napi_set_element(env, list, at++, item);
    }
    for (uint32_t index = 0; made && index < wake_count; index += 1u) {
        napi_value wake = NULL;
        napi_value when = NULL;
        if (napi_create_object(env, &wake) != napi_ok) {
            continue;
        }
        tna_put(env, wake, "address", tna_address_object(env, &wakes[index].address));
        if (wakes[index].has_at) {
            napi_create_double(env, (double)wakes[index].at, &when);
        } else {
            napi_get_null(env, &when);
        }
        tna_put(env, wake, "at", when);
        napi_set_element(env, wake_list, index, wake);
    }
    /* The bytes are copied into JavaScript's buffers: the entries' go back for reuse. */
    tah_inputs_release(inputs);
    if (made) {
        tna_put(env, batch, "entries", list);
        tna_put(env, batch, "wakes", wake_list);
    }
    tna_env *data = wrapper->env_data;
    napi_value handler = NULL;
    napi_value argv[3] = { NULL, NULL, batch };
    bool callable = made && data->effects_handler != NULL && wrapper->facade != NULL &&
                    napi_get_reference_value(env, data->effects_handler, &handler) == napi_ok && handler != NULL &&
                    napi_get_reference_value(env, wrapper->facade, &argv[0]) == napi_ok && argv[0] != NULL &&
                    napi_create_string_utf8(env, "inputs", NAPI_AUTO_LENGTH, &argv[1]) == napi_ok;
    if (callable) {
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
    if (scope != NULL) {
        napi_close_handle_scope(env, scope);
    }
}

napi_value tna_set_input_log_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    bool is_array = false;
    uint32_t count = 0;
    if (argc >= 1 && argv[0] != NULL && napi_is_array(env, argv[0], &is_array) == napi_ok && is_array) {
        napi_get_array_length(env, argv[0], &count);
    }
    ta_arena *arena = tah_scratch_call(tah_system_scratch(system));
    ta_str *families = count == 0 ? NULL : ta_arena_alloc(arena, (size_t)count * sizeof(ta_str), _Alignof(ta_str));
    if (count != 0 && families == NULL) {
        return tna_throw(env, "out-of-memory", "setInputLog: cannot read the families");
    }
    for (uint32_t at = 0; at < count; at += 1u) {
        napi_value item = NULL;
        if (napi_get_element(env, argv[0], at, &item) != napi_ok ||
            !tna_text_argument(env, system, item, &families[at])) {
            tah_scratch_reset_call(tah_system_scratch(system));
            return tna_throw(env, "invalid", "setInputLog: families must be strings");
        }
    }
    ta_result result = tah_inputs_configure(system, families, count);
    tah_scratch_reset_call(tah_system_scratch(system));
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "setInputLog: cannot keep the families");
    }
    return NULL;
}

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
 * writeEvicted(slot, generation, wake?): the logged actor is about to be
 * evicted (napi ADR 0032 section 8): its `evicted` record, and its wake
 * fixed at `wake` (a number) or none.
 */
napi_value tna_write_evicted_method(napi_env env, napi_callback_info info) {
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
    ta_actor_id actor = 0;
    if (!tna_actor_arguments(env, argv, argc, &actor)) {
        return tna_throw(env, "invalid", "writeEvicted: expected (slot, generation, wake?)");
    }
    double wake = 0;
    napi_valuetype type = napi_undefined;
    bool has_wake = argc >= 3 && argv[2] != NULL && napi_typeof(env, argv[2], &type) == napi_ok &&
                    type == napi_number && napi_get_value_double(env, argv[2], &wake) == napi_ok && wake >= 0;
    tah_recorder_evicted(system, actor, has_wake, (ta_time)wake);
    if (!tah_inputs_enabled(tah_system_inputs(system))) {
        return NULL;
    }
    ta_result result = tah_inputs_evicted(system, actor, has_wake, (ta_time)wake);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "evict(): the input log cannot record the eviction");
    }
    return NULL;
}

/* inputForget(slot, generation): the logged actor is about to be destroyed;
 * nothing will need its address back (napi ADR 0033). */
napi_value tna_input_forget_method(napi_env env, napi_callback_info info) {
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
    ta_actor_id actor = 0;
    if (!tna_actor_arguments(env, argv, argc, &actor)) {
        return tna_throw(env, "invalid", "inputForget: expected (slot, generation)");
    }
    /* `destroyed`, written before the actor goes (kernel ADR 0002): an
     * actor whose end cannot be recorded is not destroyed. */
    tah_recorder_forget(system, actor);
    ta_result result = tah_inputs_destroying(system, actor);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "destroy(): the input log cannot record the end of the actor");
    }
    return NULL;
}

/* withdrawSend(slot, generation, send): the operator's withdrawal, recorded
 * in the input log ahead of core's (kernel ADR 0002). */
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

/* abandon(slot, generation): the operator abandons the actor's macrostep,
 * recorded in the input log ahead of core's call (kernel ADR 0002). */
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
