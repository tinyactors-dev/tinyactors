#include "napi_replay.h"
#include "napi_trace.h"

#include <stdio.h>
#include <string.h>

#include "tinyactors_host.h"
#include "napi_system.h"
#include "pump.h"
#include "runtime.h"

static napi_value tna_replay_error(napi_env env, ta_result result, const char *what) {
    char message[160];
    snprintf(message, sizeof message, "replay: %s (%s)", what, tna_result_code(result));
    return tna_throw(env, tna_result_code(result), message);
}

/* ------------------------------------------------------------------------
 * Replaying events (kernel ADR 0006 section 3)
 */

/* The text property `key` of `object`: absent (undefined, null or "") is an empty text. */
static bool tna_replayed_text(napi_env env, tah_system *system, napi_value object, const char *key, ta_str *out) {
    *out = (ta_str){ 0 };
    napi_value value = NULL;
    napi_valuetype type = napi_undefined;
    if (napi_get_named_property(env, object, key, &value) != napi_ok || napi_typeof(env, value, &type) != napi_ok) {
        return false;
    }
    if (type == napi_undefined || type == napi_null) {
        return true;
    }
    return tna_text_argument(env, system, value, out);
}

/* An ActorEvent's `type`: "external" (or absent), "internal" or "platform". */
static bool tna_replayed_type(napi_env env, napi_value object, ta_event_type *out) {
    char text[16] = { 0 };
    size_t length = 0;
    napi_value value = NULL;
    napi_valuetype type = napi_undefined;
    *out = TA_EVENT_EXTERNAL;
    if (napi_get_named_property(env, object, "type", &value) != napi_ok || napi_typeof(env, value, &type) != napi_ok) {
        return false;
    }
    if (type == napi_undefined) {
        return true;
    }
    if (type != napi_string || napi_get_value_string_utf8(env, value, text, sizeof text, &length) != napi_ok) {
        return false;
    }
    if (strcmp(text, "external") == 0) {
        return true;
    }
    if (strcmp(text, "internal") == 0) {
        *out = TA_EVENT_INTERNAL;
        return true;
    }
    if (strcmp(text, "platform") == 0) {
        *out = TA_EVENT_PLATFORM;
        return true;
    }
    return false;
}

/*
 * replayEvent(slot, generation, event, at): `event`, an ActorEvent, taken
 * again at `at` by the replaying actor, whose macrostep runs to its end.
 * Returns { consumed, finished }.
 */
static napi_value tna_replay_event_inner(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = { NULL, NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    if (tah_system_inside_core(system)) {
        return tna_throw(env, "reentrant", "replay: cannot be called from inside a core call (ADR 0013)");
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    double at = 0;
    napi_valuetype kind = napi_undefined;
    if (argc < 4 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok || napi_typeof(env, argv[2], &kind) != napi_ok ||
        kind != napi_object || napi_get_value_double(env, argv[3], &at) != napi_ok) {
        return tna_throw(env, "invalid", "replayEvent: expected (slot, generation, event, at)");
    }
    tah_replay_event_params params = { .actor = ta_actor_id_make(slot, generation), .time = (ta_time)at };
    bool texts = tna_replayed_text(env, system, argv[2], "name", &params.name) &&
                 tna_replayed_text(env, system, argv[2], "sendID", &params.send_id) &&
                 tna_replayed_text(env, system, argv[2], "origin", &params.origin) &&
                 tna_replayed_text(env, system, argv[2], "originType", &params.origin_type) &&
                 tna_replayed_text(env, system, argv[2], "invocationID", &params.invoke_id);
    if (!texts || params.name.len == 0 || !tna_replayed_type(env, argv[2], &params.type)) {
        tah_scratch_reset_call(tah_system_scratch(system));
        return tna_throw(env, "invalid",
                         "replay(): the event is an ActorEvent: a name, a type of \"external\", \"internal\" or "
                         "\"platform\", and texts or nothing for sendID, origin, originType and invocationID");
    }
    napi_value data = NULL;
    napi_valuetype data_type = napi_undefined;
    ta_value value = { 0 };
    if (napi_get_named_property(env, argv[2], "data", &data) != napi_ok ||
        napi_typeof(env, data, &data_type) != napi_ok) {
        tah_scratch_reset_call(tah_system_scratch(system));
        return NULL;
    }
    if (data_type != napi_undefined && !tna_value_make(&wrapper->values, data, &value)) {
        tah_scratch_reset_call(tah_system_scratch(system));
        return tna_throw(env, "out-of-memory", "replay(): cannot keep the event's data");
    }
    params.data = data_type != napi_undefined ? &value : NULL;
    bool consumed = false;
    bool finished = false;
    params.consumed = &consumed;
    params.finished = &finished;
    ta_result result = tah_replay_event(system, &params);
    ta_value_release(&value);
    tah_scratch_reset_call(tah_system_scratch(system));
    if (result != TA_OK) {
        return tna_replay_error(env, result, "the event cannot be replayed");
    }
    napi_value out = NULL;
    napi_value flag = NULL;
    napi_create_object(env, &out);
    napi_get_boolean(env, consumed, &flag);
    napi_set_named_property(env, out, "consumed", flag);
    napi_get_boolean(env, finished, &flag);
    napi_set_named_property(env, out, "finished", flag);
    return out;
}

napi_value tna_replay_event_method(napi_env env, napi_callback_info info) {
    return tna_trace_after_method(env, info, tna_replay_event_inner(env, info));
}

/* replayEventsDone(slot, generation): the replaying actor goes back to the scheduler before publish(). */
napi_value tna_replay_events_done_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (argc < 2 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "replayEventsDone: expected (slot, generation)");
    }
    ta_result result = tah_replay_events_done(wrapper->system, ta_actor_id_make(slot, generation));
    if (result != TA_OK) {
        return tna_replay_error(env, result, "the actor cannot be published");
    }
    return NULL;
}
