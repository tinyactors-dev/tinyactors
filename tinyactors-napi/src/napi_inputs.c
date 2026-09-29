#include "napi_inputs.h"

#include <string.h>

#include "inputs.h"
#include "pump.h"
#include "runtime.h"
#include "values.h"

static void tna_put(napi_env env, napi_value object, const char *name, napi_value value) {
    if (value != NULL) {
        napi_set_named_property(env, object, name, value);
    }
}

static napi_value tna_text(napi_env env, ta_str text) {
    napi_value out = NULL;
    if (text.ptr != NULL) {
        napi_create_string_utf8(env, text.ptr, text.len, &out);
    }
    return out;
}

static napi_value tna_input_number(napi_env env, double value) {
    napi_value out = NULL;
    napi_create_double(env, value, &out);
    return out;
}

static const char *tna_event_type_name(ta_event_type type) {
    switch (type) {
    case TA_EVENT_PLATFORM: return "platform";
    case TA_EVENT_INTERNAL: return "internal";
    default: return "external";
    }
}

/* One record as the object lib/system.js reads (index.d.ts, InputRecord);
 * `slot` and `generation` name the actor for lib, which removes them. */
static napi_value tna_input_object(napi_env env, tna_system *system, const tna_input *input) {
    napi_value object = NULL;
    if (napi_create_object(env, &object) != napi_ok) {
        return NULL;
    }
    napi_value kind = NULL;
    napi_create_string_utf8(env, tna_input_kind_name(input->kind), NAPI_AUTO_LENGTH, &kind);
    tna_put(env, object, "kind", kind);
    tna_put(env, object, "time", tna_input_number(env, (double)input->time));
    tna_put(env, object, "address", tna_address_object(env, &input->address));
    if (input->actor != 0) {
        tna_put(env, object, "slot", tna_input_number(env, ta_actor_id_slot(input->actor)));
        tna_put(env, object, "generation", tna_input_number(env, ta_actor_id_generation(input->actor)));
    }
    if (input->seq != 0) {
        tna_put(env, object, "seq", tna_input_number(env, (double)input->seq));
    }
    if (input->kind == TNA_INPUT_QUEUED || input->kind == TNA_INPUT_EVENT || input->kind == TNA_INPUT_JUMP) {
        tna_put(env, object, "name", tna_event_text(env, system, input->name));
        napi_value type = NULL;
        napi_create_string_utf8(env, tna_event_type_name(input->type), NAPI_AUTO_LENGTH, &type);
        tna_put(env, object, "type", type);
        tna_put(env, object, "origin", tna_text(env, input->origin));
        tna_put(env, object, "originType", tna_text(env, input->origin_type));
    }
    tna_put(env, object, "sendID", tna_text(env, input->send_id));
    tna_put(env, object, "invokeID", tna_text(env, input->invoke_id));
    if (input->kind == TNA_INPUT_REPORT) {
        tna_put(env, object, "reason", tna_text(env, input->reason));
        napi_value ends = NULL;
        napi_get_boolean(env, input->ends, &ends);
        tna_put(env, object, "ends", ends);
    }
    if (input->kind == TNA_INPUT_SPAWNED) {
        tna_put(env, object, "revision", tna_text(env, input->revision));
    }
    if (input->kind == TNA_INPUT_JUMP) {
        /* State IDs and the definition they belong to: lib names them. */
        napi_value definition = NULL;
        napi_value targets = NULL;
        napi_create_bigint_uint64(env, input->definition, &definition);
        tna_put(env, object, "definition", definition);
        tna_put(env, object, "source", tna_input_number(env, input->source));
        if (napi_create_array_with_length(env, input->target_count, &targets) == napi_ok) {
            for (uint32_t at = 0; at < input->target_count; at += 1u) {
                napi_set_element(env, targets, at, tna_input_number(env, input->targets[at]));
            }
            tna_put(env, object, "targets", targets);
        }
    }
    if (input->has_value) {
        napi_value value = NULL;
        if (tna_value_get(env, &input->value, &value)) {
            tna_put(env, object, input->kind == TNA_INPUT_SPAWNED ? "binding" : "data", value);
        }
    }
    return object;
}

void tna_inputs_dispatch(napi_env env, tna_napi_system *wrapper, napi_value *first_error) {
    tna_system *system = wrapper->system;
    tna_inputs *inputs = &system->inputs;
    if (!inputs->enabled && inputs->count == 0 && inputs->touched.count == 0) {
        return;
    }
    napi_handle_scope scope = NULL;
    napi_open_handle_scope(env, &scope);
    uint32_t count = 0;
    tna_input *first = tna_inputs_take(inputs, &count);
    uint32_t wake_count = 0;
    const tna_input_wake *wakes = tna_inputs_wakes(system, &wake_count);
    napi_value batch = NULL;
    napi_value list = NULL;
    napi_value wake_list = NULL;
    bool made = napi_create_object(env, &batch) == napi_ok && napi_create_array_with_length(env, count, &list) == napi_ok &&
                napi_create_array_with_length(env, wake_count, &wake_list) == napi_ok;
    uint32_t at = 0;
    for (const tna_input *input = first; made && input != NULL; input = input->next) {
        napi_value object = tna_input_object(env, system, input);
        if (object != NULL) {
            napi_set_element(env, list, at++, object);
        }
    }
    for (uint32_t index = 0; made && index < wake_count; index += 1u) {
        napi_value wake = NULL;
        napi_value when = NULL;
        if (napi_create_object(env, &wake) != napi_ok) {
            continue;
        }
        tna_put(env, wake, "address", tna_address_object(env, &wakes[index].address));
        if (wakes[index].has_at) {
            when = tna_input_number(env, (double)wakes[index].at);
        } else {
            napi_get_null(env, &when);
        }
        tna_put(env, wake, "at", when);
        napi_set_element(env, wake_list, index, wake);
    }
    /* Before JavaScript runs: the values are JavaScript values now. */
    tna_inputs_release(inputs, first);
    if (made) {
        tna_put(env, batch, "inputs", list);
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
    tna_system *system = wrapper->system;
    bool is_array = false;
    uint32_t count = 0;
    if (argc >= 1 && argv[0] != NULL && napi_is_array(env, argv[0], &is_array) == napi_ok && is_array) {
        napi_get_array_length(env, argv[0], &count);
    }
    ta_str *families = count == 0 ? NULL
                                  : ta_arena_alloc(&system->scratch.call, (size_t)count * sizeof(ta_str), _Alignof(ta_str));
    if (count != 0 && families == NULL) {
        return tna_throw(env, "out-of-memory", "setInputLog: cannot read the families");
    }
    for (uint32_t at = 0; at < count; at += 1u) {
        napi_value item = NULL;
        if (napi_get_element(env, argv[0], at, &item) != napi_ok ||
            !tna_text_argument(env, system, item, &families[at])) {
            tna_scratch_reset_call(&system->scratch);
            return tna_throw(env, "invalid", "setInputLog: families must be strings");
        }
    }
    ta_result result = tna_inputs_configure(system, families, count);
    tna_scratch_reset_call(&system->scratch);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "setInputLog: cannot keep the families");
    }
    return NULL;
}

static bool tna_input_kind_from(const char *name, tna_input_kind *out) {
    static const struct {
        const char *name;
        tna_input_kind kind;
    } kinds[] = {
        { "spawned", TNA_INPUT_SPAWNED },     { "withdrawn", TNA_INPUT_WITHDRAWN }, { "abandoned", TNA_INPUT_ABANDONED },
        { "released", TNA_INPUT_RELEASED },   { "evicted", TNA_INPUT_EVICTED },
    };
    for (size_t at = 0; at < sizeof kinds / sizeof kinds[0]; at += 1u) {
        if (strcmp(kinds[at].name, name) == 0) {
            *out = kinds[at].kind;
            return true;
        }
    }
    return false;
}

napi_value tna_input_host_method(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value argv[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (!system->inputs.enabled) {
        return NULL;
    }
    char kind_text[16];
    size_t length = 0;
    uint32_t slot = 0;
    uint32_t generation = 0;
    tna_input_kind kind = TNA_INPUT_SPAWNED;
    if (argc < 3 || napi_get_value_string_utf8(env, argv[0], kind_text, sizeof kind_text, &length) != napi_ok ||
        !tna_input_kind_from(kind_text, &kind) || napi_get_value_uint32(env, argv[1], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[2], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "inputHost: expected (kind, slot, generation, value?, text?, wake?)");
    }
    tna_input_host host = { .kind = kind, .actor = ta_actor_id_make(slot, generation) };
    napi_valuetype type = napi_undefined;
    ta_value value = { 0 };
    bool has_value = argc >= 4 && argv[3] != NULL && napi_typeof(env, argv[3], &type) == napi_ok && type != napi_undefined;
    if (has_value && !tna_value_make(&wrapper->values, argv[3], &value)) {
        return tna_throw(env, "out-of-memory", "inputHost: cannot keep the value");
    }
    host.value = has_value ? &value : NULL;
    bool ok = true;
    if (argc >= 5 && argv[4] != NULL && napi_typeof(env, argv[4], &type) == napi_ok && type == napi_string) {
        ok = tna_text_argument(env, system, argv[4], &host.text);
    }
    double wake = 0;
    if (ok && argc >= 6 && argv[5] != NULL && napi_typeof(env, argv[5], &type) == napi_ok && type == napi_number &&
        napi_get_value_double(env, argv[5], &wake) == napi_ok && wake >= 0) {
        host.has_wake = true;
        host.wake = (ta_time)wake;
    }
    if (ok) {
        tna_inputs_host(system, &host);
    }
    tna_scratch_reset_call(&system->scratch);
    if (has_value) {
        ta_value_release(&value);
    }
    return ok ? NULL : tna_throw(env, "invalid", "inputHost: expected text");
}
