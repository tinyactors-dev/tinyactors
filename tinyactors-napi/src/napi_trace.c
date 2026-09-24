#include "napi_trace.h"

#include <string.h>

#include "adapters/trace.h"
#include "pump.h"

/* ------------------------------------------------------------------------
 * Names
 */

/* ta_trace_kind, in order: the names JavaScript sees and masks with. */
static const char *const tna_trace_kinds[] = {
    "spawned",        "loaded",   "unloaded",  "finished",   "destroyed",      "macrostepBegin",
    "macrostepEnd",   "condition", "microstep", "raised",     "dataInit",       "assign",
    "script",         "foreach",  "actionFailed", "sent",    "cancelled",      "invoked",
    "invokeCancelled", "paused",  "resumed",   "stopped",
};
_Static_assert(sizeof tna_trace_kinds / sizeof tna_trace_kinds[0] == (size_t)TA_TRACE_STOPPED + 1u,
               "one name per ta_trace_kind");

static const char *const tna_trace_ats[] = {
    "none", "select", "exit", "transition", "enter", "bootstrap", "finalize", "invoke",
};
_Static_assert(sizeof tna_trace_ats / sizeof tna_trace_ats[0] == (size_t)TA_TRACE_AT_INVOKE + 1u,
               "one name per ta_trace_at");

static const char *const tna_event_types[] = { "external", "internal", "platform" };

/* ------------------------------------------------------------------------
 * Building record objects
 */

static void tna_js_put(napi_env env, napi_value object, const char *name, napi_value value) {
    if (value != NULL) {
        napi_set_named_property(env, object, name, value);
    }
}

static napi_value tna_js_string(napi_env env, const char *ptr, size_t len) {
    napi_value out = NULL;
    napi_create_string_utf8(env, ptr == NULL ? "" : ptr, ptr == NULL ? 0 : len, &out);
    return out;
}

static napi_value tna_js_number(napi_env env, double value) {
    napi_value out = NULL;
    napi_create_double(env, value, &out);
    return out;
}

static napi_value tna_js_bigint(napi_env env, uint64_t value) {
    napi_value out = NULL;
    napi_create_bigint_uint64(env, value, &out);
    return out;
}

static napi_value tna_js_boolean(napi_env env, bool value) {
    napi_value out = NULL;
    napi_get_boolean(env, value, &out);
    return out;
}

static bool tna_js_key_empty(const ta_step_key *key) {
    return key->session == 0 && key->macrostep == 0 && key->microstep == 0;
}

static napi_value tna_js_key_object(napi_env env, const ta_step_key *key) {
    napi_value out = NULL;
    napi_create_object(env, &out);
    tna_js_put(env, out, "session", tna_js_bigint(env, key->session));
    tna_js_put(env, out, "macrostep", tna_js_bigint(env, key->macrostep));
    tna_js_put(env, out, "microstep", tna_js_number(env, key->microstep));
    return out;
}

napi_value tna_trace_parent_object(napi_env env, const ta_trace_parent *parent) {
    if (parent->context.trace_id_high == 0 && parent->context.trace_id_low == 0 && tna_js_key_empty(&parent->cause)) {
        napi_value undefined = NULL;
        napi_get_undefined(env, &undefined);
        return undefined;
    }
    napi_value out = NULL;
    napi_create_object(env, &out);
    tna_js_put(env, out, "traceIdHigh", tna_js_bigint(env, parent->context.trace_id_high));
    tna_js_put(env, out, "traceIdLow", tna_js_bigint(env, parent->context.trace_id_low));
    tna_js_put(env, out, "flags", tna_js_number(env, parent->context.flags));
    tna_js_put(env, out, "cause", tna_js_key_object(env, &parent->cause));
    return out;
}

/* The trace-id as 32 lowercase hex digits. */
static napi_value tna_trace_id(napi_env env, const ta_trace_context *context) {
    static const char digits[] = "0123456789abcdef";
    char text[32];
    uint64_t halves[2] = { context->trace_id_high, context->trace_id_low };
    for (int half = 0; half < 2; half += 1) {
        for (int nibble = 0; nibble < 16; nibble += 1) {
            text[half * 16 + nibble] = digits[(halves[half] >> (60 - 4 * nibble)) & 0xfu];
        }
    }
    return tna_js_string(env, text, sizeof text);
}

/* A state's author ID, or its number when its definition is gone. */
static napi_value tna_js_state(napi_env env, tna_system *system, ta_definition_id definition, ta_state_id state) {
    ta_state_info info = { 0 };
    if (ta_definition_state(system->core, &(ta_definition_state_params){ .definition = definition, .state = state,
                                                                          .out = &info }) == TA_OK) {
        return tna_js_string(env, info.id.ptr, info.id.len);
    }
    return tna_js_number(env, state);
}

static napi_value tna_js_states(napi_env env, tna_system *system, const ta_trace_record *record, const ta_state_id *states,
                             uint32_t count) {
    napi_value out = NULL;
    napi_create_array_with_length(env, count, &out);
    for (uint32_t index = 0; index < count; index += 1u) {
        napi_set_element(env, out, index, tna_js_state(env, system, record->definition, states[index]));
    }
    return out;
}

static napi_value tna_js_ids(napi_env env, const uint32_t *ids, uint32_t count) {
    napi_value out = NULL;
    napi_create_array_with_length(env, count, &out);
    for (uint32_t index = 0; index < count; index += 1u) {
        napi_set_element(env, out, index, tna_js_number(env, ids[index]));
    }
    return out;
}

/* A send ID's text (core ADR 0048), or undefined for none. */
static napi_value tna_js_send_text(napi_env env, tna_system *system, ta_send_id send) {
    char text[128];
    uint32_t needed = 0;
    if (send == 0 || ta_send_id_format(system->core, &(ta_send_id_format_params){ .send_id = send, .buffer = text,
                                                                                    .capacity = sizeof text,
                                                                                    .needed = &needed }) != TA_OK) {
        return NULL;
    }
    return tna_js_string(env, text, needed > 0 ? needed - 1u : 0);
}

static napi_value tna_js_invoke_text(napi_env env, tna_system *system, ta_invoke_id invoke) {
    ta_str text = { 0 };
    if (ta_system_invoke_id_text(system->core, invoke, &text) != TA_OK) {
        return NULL;
    }
    return tna_js_string(env, text.ptr, text.len);
}

static napi_value tna_js_value(napi_env env, const ta_value *value) {
    napi_value out = NULL;
    if (!tna_value_get(env, value, &out)) {
        napi_get_undefined(env, &out);
    }
    return out;
}

static napi_value tna_js_event(napi_env env, tna_system *system, const ta_event *event) {
    napi_value out = NULL;
    napi_create_object(env, &out);
    tna_js_put(env, out, "name", tna_event_text(env, system, event->name));
    tna_js_put(env, out, "type", tna_js_string(env, tna_event_types[event->type <= TA_EVENT_PLATFORM ? event->type : 0],
                                         strlen(tna_event_types[event->type <= TA_EVENT_PLATFORM ? event->type : 0])));
    tna_js_put(env, out, "sendId", tna_js_send_text(env, system, event->send_id));
    if (event->invoke_id != TA_INVOKE_ID_NONE) {
        tna_js_put(env, out, "invokeId", tna_js_invoke_text(env, system, event->invoke_id));
    }
    if (event->data.v != NULL) {
        tna_js_put(env, out, "data", tna_js_value(env, &event->data));
    }
    return out;
}

static napi_value tna_js_fault(napi_env env, tna_system *system, const ta_fault *fault) {
    napi_value out = NULL;
    napi_create_object(env, &out);
    tna_js_put(env, out, "code", tna_js_number(env, fault->code));
    if (fault->detail != 0) {
        tna_js_put(env, out, "detail", tna_event_text(env, system, fault->detail));
    }
    return out;
}

static napi_value tna_js_record(napi_env env, tna_napi_system *wrapper, const tna_trace_entry *entry) {
    tna_system *system = wrapper->system;
    const ta_trace_record *record = &entry->record;
    napi_value out = NULL;
    if (napi_create_object(env, &out) != napi_ok) {
        return NULL;
    }
    const char *kind = tna_trace_kinds[record->kind <= TA_TRACE_STOPPED ? record->kind : 0];
    tna_js_put(env, out, "kind", tna_js_string(env, kind, strlen(kind)));
    tna_js_put(env, out, "session", tna_js_bigint(env, record->key.session));
    tna_js_put(env, out, "macrostep", tna_js_number(env, (double)record->key.macrostep));
    tna_js_put(env, out, "microstep", tna_js_number(env, record->key.microstep));
    tna_js_put(env, out, "actor", tna_actor_view(env, wrapper, record->actor));
    tna_js_put(env, out, "now", tna_js_number(env, (double)record->now));
    tna_js_put(env, out, "wallTime", tna_js_number(env, entry->wall_time));
    const ta_trace_position *position = &record->position;
    if (position->at != TA_TRACE_AT_NONE && position->at <= TA_TRACE_AT_INVOKE) {
        const char *at = tna_trace_ats[position->at];
        tna_js_put(env, out, "at", tna_js_string(env, at, strlen(at)));
        if (position->state != TA_STATE_NONE) {
            tna_js_put(env, out, "state", tna_js_state(env, system, record->definition, position->state));
        }
        if (position->transition != TA_TRANSITION_NONE) {
            tna_js_put(env, out, "transition", tna_js_number(env, position->transition));
        }
        if (position->action != TA_ACTION_NONE) {
            tna_js_put(env, out, "action", tna_js_number(env, position->action));
        }
        tna_js_put(env, out, "iteration", tna_js_number(env, position->iteration));
    }
    if (record->context.trace_id_high != 0 || record->context.trace_id_low != 0) {
        tna_js_put(env, out, "traceId", tna_trace_id(env, &record->context));
        tna_js_put(env, out, "sampled", tna_js_boolean(env, (record->context.flags & TA_TRACE_FLAG_SAMPLED) != 0));
    }
    if ((record->kind == TA_TRACE_MACROSTEP_BEGIN || record->kind == TA_TRACE_SPAWNED) &&
        !tna_js_key_empty(&record->cause)) {
        tna_js_put(env, out, "cause", tna_js_key_object(env, &record->cause));
    }
    if (record->event != NULL) {
        tna_js_put(env, out, "event", tna_js_event(env, system, record->event));
    }
    if (record->has_fault) {
        tna_js_put(env, out, "fault", tna_js_fault(env, system, &record->fault));
    }
    switch (record->kind) {
        case TA_TRACE_FINISHED:
            if (record->state != TA_STATE_NONE) {
                tna_js_put(env, out, "finalState", tna_js_state(env, system, record->definition, record->state));
            }
            break;
        case TA_TRACE_MACROSTEP_END:
            tna_js_put(env, out, "states", tna_js_states(env, system, record, record->states, record->state_count));
            break;
        case TA_TRACE_CONDITION:
            tna_js_put(env, out, "expr", tna_js_number(env, record->expr));
            if (!record->has_fault) {
                tna_js_put(env, out, "result", tna_js_boolean(env, record->result));
            }
            break;
        case TA_TRACE_MICROSTEP:
            tna_js_put(env, out, "transitions", tna_js_ids(env, record->transitions, record->transition_count));
            tna_js_put(env, out, "exited", tna_js_states(env, system, record, record->exited, record->exited_count));
            tna_js_put(env, out, "entered", tna_js_states(env, system, record, record->entered, record->entered_count));
            if (record->history_count != 0) {
                tna_js_put(env, out, "history", tna_js_states(env, system, record, record->history, record->history_count));
            }
            break;
        case TA_TRACE_DATA_INIT:
            tna_js_put(env, out, "name", tna_js_string(env, record->name.ptr, record->name.len));
            /* fall through: its expression and value */
        case TA_TRACE_ASSIGN:
        case TA_TRACE_SCRIPT:
            if (record->expr != TA_EXPR_ID_NONE) {
                tna_js_put(env, out, "expr", tna_js_number(env, record->expr));
            }
            if (record->has_value) {
                tna_js_put(env, out, "value", tna_js_value(env, &record->value));
            }
            break;
        case TA_TRACE_FOREACH:
            tna_js_put(env, out, "iteration", tna_js_number(env, record->iteration));
            if (record->has_value) {
                tna_js_put(env, out, "value", tna_js_value(env, &record->value));
            }
            break;
        case TA_TRACE_SENT:
        case TA_TRACE_CANCELLED:
            tna_js_put(env, out, "sendId", tna_js_send_text(env, system, record->send));
            break;
        case TA_TRACE_INVOKED:
        case TA_TRACE_INVOKE_CANCELLED:
            tna_js_put(env, out, "invokeId", tna_js_invoke_text(env, system, record->invoke));
            break;
        default:
            break;
    }
    return out;
}

/* ------------------------------------------------------------------------
 * Dispatch
 */

void tna_trace_dispatch(napi_env env, tna_napi_system *wrapper, napi_value *first_error) {
    tna_system *system = wrapper->system;
    if (tna_traces_count(&system->traces) == 0) {
        return;
    }
    uint32_t count = 0;
    tna_trace_entry *entries = tna_traces_view(&system->traces, &count);
    napi_handle_scope scope = NULL;
    napi_open_handle_scope(env, &scope);
    tna_env *data = wrapper->env_data;
    napi_value handler = NULL;
    napi_value argv[3] = { NULL, NULL, NULL };
    bool callable = data->effects_handler != NULL && wrapper->facade != NULL &&
                    napi_get_reference_value(env, data->effects_handler, &handler) == napi_ok && handler != NULL &&
                    napi_get_reference_value(env, wrapper->facade, &argv[0]) == napi_ok && argv[0] != NULL &&
                    napi_create_string_utf8(env, "trace", NAPI_AUTO_LENGTH, &argv[1]) == napi_ok &&
                    napi_create_array_with_length(env, count, &argv[2]) == napi_ok;
    uint32_t index = 0;
    for (uint32_t at = 0; callable && at < count; at += 1u) {
        napi_value record = tna_js_record(env, wrapper, &entries[at]);
        if (record != NULL) {
            napi_set_element(env, argv[2], index++, record);
        }
    }
    /* Converted: what the records retained goes back before JavaScript runs,
     * so a hook that closes the system finds nothing left to release. */
    tna_traces_clear(&system->traces);
    if (callable && index != 0) {
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

/* ------------------------------------------------------------------------
 * Installing the sink, and trace parents from JavaScript
 */

napi_value tna_set_trace_sink_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "setTraceSink cannot be called from inside a core call (ADR 0013)");
    }
    bool installed = false;
    uint32_t kinds = 0;
    bool unsampled = false;
    if (argc < 3 || napi_get_value_bool(env, argv[0], &installed) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &kinds) != napi_ok ||
        napi_get_value_bool(env, argv[2], &unsampled) != napi_ok) {
        return tna_throw(env, "invalid", "setTraceSink: expected (installed, kinds, unsampled)");
    }
    ta_trace_sink sink = installed && kinds != 0 ? tna_trace_sink_adapter(system, kinds, unsampled) : ta_null_trace_sink();
    ta_result result = ta_system_set_trace_sink(system->core, sink);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "setTraceSink: core refused the sink");
    }
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

static bool tna_bigint_member(napi_env env, napi_value object, const char *name, uint64_t *out) {
    napi_value value = NULL;
    bool lossless = false;
    return napi_get_named_property(env, object, name, &value) == napi_ok &&
           napi_get_value_bigint_uint64(env, value, out, &lossless) == napi_ok && lossless;
}

bool tna_trace_argument(napi_env env, napi_value value, ta_trace_parent *out) {
    *out = (ta_trace_parent){ 0 };
    napi_valuetype type = napi_undefined;
    if (value == NULL || napi_typeof(env, value, &type) != napi_ok || type == napi_undefined) {
        return true;
    }
    uint32_t flags = 0;
    napi_value member = NULL;
    if (type != napi_object || !tna_bigint_member(env, value, "high", &out->context.trace_id_high) ||
        !tna_bigint_member(env, value, "low", &out->context.trace_id_low) ||
        !tna_bigint_member(env, value, "span", &out->cause.macrostep) ||
        napi_get_named_property(env, value, "flags", &member) != napi_ok ||
        napi_get_value_uint32(env, member, &flags) != napi_ok || flags > 0xffu) {
        tna_throw(env, "invalid", "post: the trace must come from a traceparent (lib/tracing.js)");
        return false;
    }
    out->context.flags = (uint8_t)flags;
    return true;
}

napi_value tna_trace_kind_names(napi_env env) {
    napi_value out = NULL;
    size_t count = sizeof tna_trace_kinds / sizeof tna_trace_kinds[0];
    napi_create_array_with_length(env, count, &out);
    for (uint32_t index = 0; index < count; index += 1u) {
        napi_set_element(env, out, index, tna_js_string(env, tna_trace_kinds[index], strlen(tna_trace_kinds[index])));
    }
    return out;
}
