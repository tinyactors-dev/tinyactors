#include "napi_recorder.h"

#include "tinyactors_host.h"
#include "pump.h"
#include "runtime.h"
#include "values.h"

static void tna_put(napi_env env, napi_value object, const char *name, napi_value value) {
    if (value != NULL) {
        napi_set_named_property(env, object, name, value);
    }
}

/* A text property, set only when the text is there. */
static void tna_put_text(napi_env env, napi_value object, const char *name, ta_str text) {
    napi_value value = NULL;
    if (text.len != 0 && napi_create_string_utf8(env, text.ptr, text.len, &value) == napi_ok) {
        napi_set_named_property(env, object, name, value);
    }
}

static const char *tna_event_type_name(ta_event_type type) {
    switch (type) {
    case TA_EVENT_INTERNAL: return "internal";
    case TA_EVENT_PLATFORM: return "platform";
    default: return "external";
    }
}

/* Record `index` as { address, event, at }, the event an ActorEvent; NULL when it cannot be made. */
static napi_value tna_record_object(napi_env env, tah_system *system, uint32_t index) {
    tah_record record = tah_recorder_record(system, index);
    tah_record_texts texts;
    ta_arena *arena = tah_scratch_call(tah_system_scratch(system));
    napi_value item = NULL;
    napi_value event = NULL;
    napi_value value = NULL;
    if (tah_recorder_texts(system, index, ta_arena_allocator(arena), &texts) != TA_OK ||
        napi_create_object(env, &item) != napi_ok || napi_create_object(env, &event) != napi_ok) {
        tah_scratch_reset_call(tah_system_scratch(system));
        return NULL;
    }
    tna_put_text(env, event, "name", texts.name);
    napi_create_string_utf8(env, tna_event_type_name(texts.type), NAPI_AUTO_LENGTH, &value);
    tna_put(env, event, "type", value);
    tna_put_text(env, event, "sendID", texts.send_id);
    tna_put_text(env, event, "origin", texts.origin);
    tna_put_text(env, event, "originType", texts.origin_type);
    tna_put_text(env, event, "invocationID", texts.invoke_id);
    value = NULL;
    if (!tna_value_get(env, &record.event->data, &value) || value == NULL) {
        napi_get_undefined(env, &value);
    }
    tna_put(env, event, "data", value);
    tah_scratch_reset_call(tah_system_scratch(system));
    tna_put(env, item, "address", tna_address_object(env, &record.address));
    tna_put(env, item, "event", event);
    napi_create_double(env, (double)record.at, &value);
    tna_put(env, item, "at", value);
    return item;
}

void tna_recorder_dispatch(napi_env env, tna_napi_system *wrapper, napi_value *first_error) {
    tah_system *system = wrapper->system;
    uint32_t count = tah_recorder_count(system);
    uint32_t wake_count = 0;
    const tah_record_wake *wakes = tah_recorder_wakes(system, &wake_count);
    if (count == 0 && wake_count == 0) {
        tah_recorder_release(system);
        return;
    }
    napi_handle_scope scope = NULL;
    napi_open_handle_scope(env, &scope);
    napi_value batch = NULL;
    napi_value records = NULL;
    napi_value wake_list = NULL;
    bool made = napi_create_object(env, &batch) == napi_ok && napi_create_array(env, &records) == napi_ok &&
                napi_create_array_with_length(env, wake_count, &wake_list) == napi_ok;
    uint32_t at = 0;
    for (uint32_t index = 0; made && index < count; index += 1u) {
        napi_value item = tna_record_object(env, system, index);
        if (item != NULL) {
            napi_set_element(env, records, at++, item);
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
            napi_create_double(env, (double)wakes[index].at, &when);
        } else {
            napi_get_null(env, &when);
        }
        tna_put(env, wake, "at", when);
        napi_set_element(env, wake_list, index, wake);
    }
    /* The events' data are JavaScript values now: the records go. */
    tah_recorder_release(system);
    if (made) {
        tna_put(env, batch, "records", records);
        tna_put(env, batch, "wakes", wake_list);
    }
    tna_env *data = wrapper->env_data;
    napi_value handler = NULL;
    napi_value argv[3] = { NULL, NULL, batch };
    bool callable = made && data->effects_handler != NULL && wrapper->facade != NULL &&
                    napi_get_reference_value(env, data->effects_handler, &handler) == napi_ok && handler != NULL &&
                    napi_get_reference_value(env, wrapper->facade, &argv[0]) == napi_ok && argv[0] != NULL &&
                    napi_create_string_utf8(env, "record", NAPI_AUTO_LENGTH, &argv[1]) == napi_ok;
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

napi_value tna_set_recorder_method(napi_env env, napi_callback_info info) {
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
        return tna_throw(env, "out-of-memory", "setRecorder: cannot read the families");
    }
    for (uint32_t at = 0; at < count; at += 1u) {
        napi_value item = NULL;
        if (napi_get_element(env, argv[0], at, &item) != napi_ok ||
            !tna_text_argument(env, system, item, &families[at])) {
            tah_scratch_reset_call(tah_system_scratch(system));
            return tna_throw(env, "invalid", "setRecorder: families must be strings");
        }
    }
    ta_result result = tah_recorder_configure(system, families, count);
    tah_scratch_reset_call(tah_system_scratch(system));
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "setRecorder: cannot keep the families");
    }
    return NULL;
}
