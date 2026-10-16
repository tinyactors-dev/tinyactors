/* The effects (AGENTS.md, "Lifetimes and thin views"; translated from
 * tinyactors-cpython/src/effects.c): dispatch of one effect of the open
 * batch, and the objects JavaScript sees of them.
 *
 * A thin view (a Send, a LogEntry) is a small object whose state is an
 * array under the instance's `view` symbol: the Host's wrapper, the ack
 * count of its batch, the effect's index, the send's I/O processor, its
 * flags (record, answered, failed), then one slot per field, a hole until
 * the field is read. A field is converted on first read and cached; a read
 * with another ack count throws TA_ERROR_STALE. When a handler returns a
 * thenable, its view is filled with every field before the ack and marked
 * a record: it never goes stale, and answer() and fail() work while the
 * host lives (docs/plan.xml, decision d-async). The state is the engine's
 * memory, as a Python view's fields are Python's: the addon allocates
 * nothing per effect. */
#include "addon.h"

#include <string.h>

/* ---- the state of a view ------------------------------------------------------- */

enum {
    TA_NAPI_VIEW_HOST,
    TA_NAPI_VIEW_ACKS,
    TA_NAPI_VIEW_INDEX,
    TA_NAPI_VIEW_PROCESSOR,
    TA_NAPI_VIEW_FLAGS,
    TA_NAPI_VIEW_FIELDS /* the first field's slot */
};

enum { TA_NAPI_VIEW_RECORD = 1u, TA_NAPI_VIEW_ANSWERED = 2u, TA_NAPI_VIEW_FAILED = 4u };

/* A Send's fields, in the order a record is filled. */
enum {
    TA_NAPI_SEND_SESSION,
    TA_NAPI_SEND_EVENT,
    TA_NAPI_SEND_TARGET,
    TA_NAPI_SEND_SENDID,
    TA_NAPI_SEND_DATA,
    TA_NAPI_SEND_TYPE,
    TA_NAPI_SEND_ORIGIN,
    TA_NAPI_SEND_TRACEPARENT,
    TA_NAPI_SEND_COUNT
};

/* A LogEntry's fields. */
enum { TA_NAPI_ENTRY_SESSION, TA_NAPI_ENTRY_LABEL, TA_NAPI_ENTRY_TEXT, TA_NAPI_ENTRY_COUNT };

/* What a getter reads, its descriptor's data: the view's kind and field. */
enum { TA_NAPI_KIND_SEND = 0, TA_NAPI_KIND_ENTRY = 1 };
#define TA_NAPI_GETTER(kind, field) ((void *)(uintptr_t)(((kind) << 8) | (field)))

/* A view as C reads it, for one call. */
typedef struct ta_napi_view {
    napi_value self, state, wrapper;
    ta_napi_host *host;
    double acks;
    uint32_t index, processor, flags;
} ta_napi_view;

static napi_value ta_napi_undefined(napi_env env) {
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

static bool ta_napi_is(napi_env env, napi_value value, napi_valuetype want) {
    napi_valuetype type = napi_undefined;
    return value != NULL && napi_typeof(env, value, &type) == napi_ok && type == want;
}

/* The instance's symbol `name`. */
static napi_value ta_napi_key(napi_env env, const char *name) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value keys, key;
    if (instance == NULL || napi_get_reference_value(env, instance->keys, &keys) != napi_ok ||
        napi_get_named_property(env, keys, name, &key) != napi_ok)
        return NULL;
    return key;
}

/* A number of a view's state: slot `slot` of `state` into `value`. */
typedef struct ta_napi_slot {
    napi_value state;
    uint32_t slot;
    uint32_t *value; /* out */
} ta_napi_slot;

static bool ta_napi_state_uint32(napi_env env, const ta_napi_slot *slot) {
    napi_value value;
    return napi_get_element(env, slot->state, slot->slot, &value) == napi_ok &&
           napi_get_value_uint32(env, value, slot->value) == napi_ok;
}

/* `self` as a view; false with a TypeError when it is none. */
static bool ta_napi_view_of(napi_env env, napi_value self, ta_napi_view *view) {
    napi_value key = ta_napi_key(env, "view"), acks;
    bool array = false;
    memset(view, 0, sizeof *view);
    view->self = self;
    if (key == NULL) return false;
    if (!ta_napi_is(env, self, napi_object) || napi_get_property(env, self, key, &view->state) != napi_ok ||
        napi_is_array(env, view->state, &array) != napi_ok || !array ||
        napi_get_element(env, view->state, TA_NAPI_VIEW_HOST, &view->wrapper) != napi_ok ||
        napi_get_element(env, view->state, TA_NAPI_VIEW_ACKS, &acks) != napi_ok ||
        napi_get_value_double(env, acks, &view->acks) != napi_ok ||
        !ta_napi_state_uint32(env, &(ta_napi_slot){view->state, TA_NAPI_VIEW_INDEX, &view->index}) ||
        !ta_napi_state_uint32(env, &(ta_napi_slot){view->state, TA_NAPI_VIEW_PROCESSOR, &view->processor}) ||
        !ta_napi_state_uint32(env, &(ta_napi_slot){view->state, TA_NAPI_VIEW_FLAGS, &view->flags})) {
        napi_value ignored;
        napi_get_and_clear_last_exception(env, &ignored);
        napi_throw_type_error(env, NULL, "not a view of an effect");
        return false;
    }
    view->host = ta_napi_host_of(env, &(ta_napi_host_check){view->wrapper, false});
    return view->host != NULL;
}

/* The flags as the state holds them now: a handler's answer() sets them
 * through a view of its own, so a copy read before it is out of date. */
static bool ta_napi_view_flags(napi_env env, ta_napi_view *view) {
    return ta_napi_state_uint32(env, &(ta_napi_slot){view->state, TA_NAPI_VIEW_FLAGS, &view->flags});
}

static bool ta_napi_view_flag(napi_env env, ta_napi_view *view, uint32_t flag) {
    napi_value flags;
    if (!ta_napi_view_flags(env, view)) return false;
    view->flags |= flag;
    return napi_create_uint32(env, view->flags, &flags) == napi_ok &&
           napi_set_element(env, view->state, TA_NAPI_VIEW_FLAGS, flags) == napi_ok;
}

/* The view's batch is still open (or it is a record); else TA_ERROR_STALE. */
static bool ta_napi_view_live(napi_env env, const ta_napi_view *view) {
    if (view->flags & TA_NAPI_VIEW_RECORD) return true;
    if (view->host->host != NULL && (double)view->host->acks == view->acks) return true;
    ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_STALE, "the effect's batch was acknowledged: read its fields in the "
                                                          "handler, or return a promise from it"});
    return false;
}

/* ---- the fields ----------------------------------------------------------------- */

/* A text field as a string; empty text as undefined when `empty_none`. */
typedef struct ta_napi_text_field {
    uint32_t field;
    bool empty_none;
} ta_napi_text_field;

static napi_value ta_napi_effect_text(napi_env env, const ta_napi_view *view, const ta_napi_text_field *field) {
    ta_string_view text = ta_host_effect_text(view->host->host, view->index, field->field);
    napi_value result = NULL;
    if (text.length == 0 && field->empty_none) return ta_napi_undefined(env);
    napi_create_string_utf8(env, text.length ? text.pointer : "", text.length, &result);
    return result;
}

static napi_value ta_napi_effect_session(napi_env env, const ta_napi_view *view) {
    napi_value result = NULL;
    napi_create_bigint_uint64(env, ta_host_effect_number(view->host->host, view->index, TA_EFFECT_FIELD_SESSION), &result);
    return result;
}

/* An address the effect gives as text, as an Address (empty: undefined). */
static napi_value ta_napi_effect_address(napi_env env, const ta_napi_view *view, uint32_t field) {
    ta_string_view text = ta_host_effect_text(view->host->host, view->index, field);
    if (text.length == 0) return ta_napi_undefined(env);
    return ta_napi_address_new(env, text);
}

/* The send's data: undefined without a payload; quickjs's decoded from its
 * CBOR (a view of quickjs's memory until its next call, read at once). The
 * native datamodel's row value comes with it (p1-datamodel). */
static napi_value ta_napi_effect_data(napi_env env, const ta_napi_view *view) {
    ta_napi_host *host = view->host;
    ta_payload payload = (ta_payload)ta_host_effect_number(host->host, view->index, TA_EFFECT_FIELD_PAYLOAD);
    uint32_t datamodel = (uint32_t)ta_host_effect_number(host->host, view->index, TA_EFFECT_FIELD_DATAMODEL);
    if (payload == 0 || datamodel == TA_NIL || host->quickjs == NULL) return ta_napi_undefined(env);
    ta_bytes cbor;
    ta_status status = ta_datamodel_ecmascript_payload_to_cbor(host->quickjs, payload, &cbor);
    if (status != TA_OK) return ta_napi_throw(env, &(ta_napi_refusal){status, "the send's data could not be read"});
    return ta_napi_value_read(env, cbor);
}

/* The handlers object on the wrapper; undefined once closed. */
static napi_value ta_napi_handlers(napi_env env, napi_value wrapper) {
    napi_value key = ta_napi_key(env, "handlers"), handlers = NULL;
    if (key == NULL || napi_get_property(env, wrapper, key, &handlers) != napi_ok) return NULL;
    return handlers;
}

/* handlers[list][index]: undefined when there is none. */
typedef struct ta_napi_handler_at {
    const char *list;
    uint32_t index;
} ta_napi_handler_at;

static napi_value ta_napi_handler_of(napi_env env, napi_value wrapper, const ta_napi_handler_at *at) {
    napi_value handlers = ta_napi_handlers(env, wrapper), list, value;
    if (handlers == NULL) return NULL;
    if (!ta_napi_is(env, handlers, napi_object)) return ta_napi_undefined(env);
    if (napi_get_named_property(env, handlers, at->list, &list) != napi_ok) return NULL;
    if (at->index == UINT32_MAX) return list; /* the handler itself (log) */
    if (!ta_napi_is(env, list, napi_object)) return ta_napi_undefined(env);
    if (napi_get_element(env, list, at->index, &value) != napi_ok) return NULL;
    return value;
}

static napi_value ta_napi_send_field(napi_env env, const ta_napi_view *view, uint32_t field) {
    switch (field) {
    case TA_NAPI_SEND_SESSION: return ta_napi_effect_session(env, view);
    case TA_NAPI_SEND_EVENT: return ta_napi_effect_text(env, view, &(ta_napi_text_field){TA_EFFECT_FIELD_EVENT, false});
    case TA_NAPI_SEND_TARGET: /* another system's address for the transport (0), else the processor's text */
        if (view->processor == 0) return ta_napi_effect_address(env, view, TA_EFFECT_FIELD_TARGET);
        return ta_napi_effect_text(env, view, &(ta_napi_text_field){TA_EFFECT_FIELD_TARGET, true});
    case TA_NAPI_SEND_SENDID: return ta_napi_effect_text(env, view, &(ta_napi_text_field){TA_EFFECT_FIELD_SENDID, true});
    case TA_NAPI_SEND_DATA: return ta_napi_effect_data(env, view);
    case TA_NAPI_SEND_TYPE: /* the config's type; undefined for the transport */
        return ta_napi_handler_of(env, view->wrapper, &(ta_napi_handler_at){"types", view->processor});
    case TA_NAPI_SEND_ORIGIN: /* the sender's address, formatted by core on first ask */
        return ta_napi_effect_address(env, view, TA_EFFECT_FIELD_ORIGIN);
    default: /* formatted by core from the binary context on first ask; empty when untraced */
        return ta_napi_effect_text(env, view, &(ta_napi_text_field){TA_EFFECT_FIELD_TRACEPARENT, true});
    }
}

static napi_value ta_napi_entry_field(napi_env env, const ta_napi_view *view, uint32_t field) {
    if (field == TA_NAPI_ENTRY_SESSION) return ta_napi_effect_session(env, view);
    uint32_t which = field == TA_NAPI_ENTRY_LABEL ? TA_EFFECT_FIELD_AUXILIARY : TA_EFFECT_FIELD_EVENT;
    return ta_napi_effect_text(env, view, &(ta_napi_text_field){which, false});
}

/* A field of a view, by kind: cached, or converted now and cached. */
typedef struct ta_napi_field {
    uint32_t kind, field;
} ta_napi_field;

static napi_value ta_napi_view_read(napi_env env, const ta_napi_view *view, const ta_napi_field *field) {
    uint32_t slot = TA_NAPI_VIEW_FIELDS + field->field;
    bool cached = false;
    napi_value value = NULL;
    if (napi_has_element(env, view->state, slot, &cached) != napi_ok) return NULL;
    if (cached) return napi_get_element(env, view->state, slot, &value) == napi_ok ? value : NULL;
    if (!ta_napi_view_live(env, view)) return NULL;
    value = field->kind == TA_NAPI_KIND_SEND ? ta_napi_send_field(env, view, field->field)
                                             : ta_napi_entry_field(env, view, field->field);
    if (value == NULL || napi_set_element(env, view->state, slot, value) != napi_ok) return NULL;
    return value;
}

/* A getter of a Send or a LogEntry. */
static napi_value ta_napi_view_get(napi_env env, napi_callback_info info) {
    size_t argc = 0;
    napi_value self = NULL;
    void *data = NULL;
    ta_napi_view view;
    if (napi_get_cb_info(env, info, &argc, NULL, &self, &data) != napi_ok || !ta_napi_view_of(env, self, &view))
        return NULL;
    uintptr_t which = (uintptr_t)data;
    return ta_napi_view_read(env, &view, &(ta_napi_field){(uint32_t)(which >> 8), (uint32_t)(which & 0xffu)});
}

static napi_value ta_napi_send_read(napi_env env, const ta_napi_view *view, uint32_t field) {
    return ta_napi_view_read(env, view, &(ta_napi_field){TA_NAPI_KIND_SEND, field});
}

/* send.message: the outgoing half of a send between systems, the object
 * host.send(message) takes on the other side (ta_event_in's fields). */
static napi_value ta_napi_send_message(napi_env env, napi_callback_info info) {
    static const char *const keys[] = {"target", "event", "data", "sendid", "origin", "traceparent"};
    static const uint32_t fields[] = {TA_NAPI_SEND_TARGET, TA_NAPI_SEND_EVENT,  TA_NAPI_SEND_DATA,
                                      TA_NAPI_SEND_SENDID, TA_NAPI_SEND_ORIGIN, TA_NAPI_SEND_TRACEPARENT};
    size_t argc = 0;
    napi_value self = NULL, message;
    ta_napi_view view;
    if (napi_get_cb_info(env, info, &argc, NULL, &self, NULL) != napi_ok || !ta_napi_view_of(env, self, &view) ||
        napi_create_object(env, &message) != napi_ok)
        return NULL;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        napi_value value = ta_napi_send_read(env, &view, fields[i]);
        if (value == NULL || napi_set_named_property(env, message, keys[i], value) != napi_ok) return NULL;
    }
    return message;
}

/* ---- answering a send ------------------------------------------------------------- */

/* The answer or the failure of a send: ta_host_send to its session with
 * its sendid (bindings.md, section 4). `name` NULL: the failure.
 * `traceparent` NULL: the send's own trace. */
typedef struct ta_napi_answer {
    napi_value name, data, traceparent, origin, raw;
} ta_napi_answer;

static bool ta_napi_send_reply(napi_env env, ta_napi_view *view, const ta_napi_answer *answer) {
    if (!ta_napi_view_flags(env, view) || !ta_napi_view_live(env, view))
        return false; /* a thin view answers before its ack, whatever was read */
    if (view->flags & (TA_NAPI_VIEW_ANSWERED | TA_NAPI_VIEW_FAILED)) {
        ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_STATE, view->flags & TA_NAPI_VIEW_FAILED
                                                                  ? "the send was failed already"
                                                                  : "the send was answered already"});
        return false;
    }
    napi_value session = ta_napi_send_read(env, view, TA_NAPI_SEND_SESSION);
    napi_value sendid = session ? ta_napi_send_read(env, view, TA_NAPI_SEND_SENDID) : NULL;
    napi_value type = sendid ? ta_napi_send_read(env, view, TA_NAPI_SEND_TYPE) : NULL;
    if (type == NULL) return false;
    napi_value traceparent = answer->traceparent;
    if (traceparent == NULL) traceparent = ta_napi_send_read(env, view, TA_NAPI_SEND_TRACEPARENT);
    ta_napi_call call;
    if (traceparent == NULL || !ta_napi_enter(env, view->wrapper, &call)) return false;
    ta_napi_host *host = call.host;
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    bool lossless = false;
    ta_napi_text sendid_text = {&host->arena, "a sendid is a string", {NULL, 0}};
    ta_napi_text trace_text = {&host->arena, "a traceparent is W3C text, a string, or null for a new trace", {NULL, 0}};
    ta_napi_text name_text = {&host->arena, "an answer's event is a string", {NULL, 0}};
    ta_napi_text type_text = {&host->arena, "a processor's type is a string", {NULL, 0}};
    ta_napi_text origin_text = {&host->arena, "an origin is an address, a string or an Address", {NULL, 0}};
    ta_napi_text raw_text = {&host->arena, "raw is the message as read, a string", {NULL, 0}};
    bool ok = napi_get_value_bigint_uint64(env, session, &event.session, &lossless) == napi_ok &&
              ta_napi_optional_text(env, sendid, &sendid_text) && ta_napi_optional_text(env, traceparent, &trace_text);
    ta_napi_payload_for payload = {event.session, {0, 0}, answer->data, 0};
    if (ok && answer->name == NULL) event.flags = TA_EVENT_IN_ERROR_COMMUNICATION; /* the name is not repeated */
    else if (ok)
        ok = ta_napi_text_of(env, answer->name, &name_text) && ta_napi_optional_text(env, type, &type_text) &&
             ta_napi_optional_text(env, ta_napi_address_text(env, answer->origin), &origin_text) &&
             ta_napi_optional_text(env, answer->raw, &raw_text) && ta_napi_host_payload(env, host, &payload);
    if (ok) {
        event.sendid = sendid_text.view;
        event.traceparent = trace_text.view;
        event.name = name_text.view;
        event.origintype = type_text.view;
        event.origin = origin_text.view;
        event.raw = raw_text.view;
        event.data = payload.payload;
        ta_status status = ta_host_send(host->host, &event);
        if (status != TA_OK) {
            ta_napi_host_payload_release(host, payload.payload); /* still ours */
            ta_napi_throw(env, &(ta_napi_refusal){status, answer->name ? "the answer was refused" : "the failure was refused"});
            ok = false;
        } else {
            ok = ta_napi_view_flag(env, view, answer->name ? TA_NAPI_VIEW_ANSWERED : TA_NAPI_VIEW_FAILED);
            ta_napi_schedule(env, host);
        }
    }
    ta_napi_leave(&call);
    return ok;
}

/* send.answer(event, data?, {traceparent, origin, raw}?) */
static napi_value ta_napi_send_answer(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {NULL, NULL, NULL}, self = NULL, keys, key;
    ta_napi_view view;
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok || !ta_napi_view_of(env, self, &view)) return NULL;
    napi_value undefined = ta_napi_undefined(env);
    for (size_t i = argc; i < 3; i++) argv[i] = undefined;
    ta_napi_answer answer = {argv[0], argv[1], NULL, undefined, undefined};
    if (!ta_napi_is(env, argv[2], napi_undefined)) {
        static const char *const known[] = {"traceparent", "origin", "raw"};
        uint32_t count = 0;
        if (!ta_napi_is(env, argv[2], napi_object) || napi_get_property_names(env, argv[2], &keys) != napi_ok ||
            napi_get_array_length(env, keys, &count) != napi_ok) {
            napi_throw_type_error(env, NULL, "an answer's options are an object of traceparent, origin, raw");
            return NULL;
        }
        for (uint32_t i = 0; i < count; i++) {
            char name[16];
            size_t length = 0;
            bool found = false;
            if (napi_get_element(env, keys, i, &key) != napi_ok ||
                napi_get_value_string_utf8(env, key, name, sizeof name, &length) != napi_ok)
                return NULL;
            for (size_t j = 0; j < 3 && !found; j++) found = strlen(known[j]) == length && memcmp(known[j], name, length) == 0;
            if (!found) {
                napi_throw_type_error(env, NULL, "an answer's options are traceparent, origin and raw");
                return NULL;
            }
        }
        napi_value traceparent;
        if (napi_get_named_property(env, argv[2], "traceparent", &traceparent) != napi_ok ||
            napi_get_named_property(env, argv[2], "origin", &answer.origin) != napi_ok ||
            napi_get_named_property(env, argv[2], "raw", &answer.raw) != napi_ok)
            return NULL;
        if (!ta_napi_is(env, traceparent, napi_undefined)) answer.traceparent = traceparent; /* null: a new trace */
    }
    if (!ta_napi_is(env, answer.name, napi_string)) {
        napi_throw_type_error(env, NULL, "answer(event, data?, options?): the event is a string");
        return NULL;
    }
    return ta_napi_send_reply(env, &view, &answer) ? undefined : NULL;
}

/* send.fail(): error.communication for the sender, with the send's sendid
 * and trace. */
static napi_value ta_napi_send_fail(napi_env env, napi_callback_info info) {
    size_t argc = 0;
    napi_value self = NULL;
    ta_napi_view view;
    if (napi_get_cb_info(env, info, &argc, NULL, &self, NULL) != napi_ok || !ta_napi_view_of(env, self, &view)) return NULL;
    ta_napi_answer answer = {NULL, ta_napi_undefined(env), NULL, NULL, NULL};
    return ta_napi_send_reply(env, &view, &answer) ? ta_napi_undefined(env) : NULL;
}

/* Fails a send nobody answered, for a handler that could not (ADR 0009: a
 * runtime that cannot do what an effect asks answers with a failure); what
 * that throws is dropped. */
static void ta_napi_send_fail_unanswered(napi_env env, ta_napi_view *view) {
    if (!ta_napi_view_flags(env, view)) {
        napi_value ignored;
        napi_get_and_clear_last_exception(env, &ignored);
        return;
    }
    if (view->flags & (TA_NAPI_VIEW_ANSWERED | TA_NAPI_VIEW_FAILED) || view->host->host == NULL) return;
    ta_napi_answer answer = {NULL, ta_napi_undefined(env), NULL, NULL, NULL};
    if (!ta_napi_send_reply(env, view, &answer)) {
        napi_value ignored;
        napi_get_and_clear_last_exception(env, &ignored);
    }
}

/* The handler threw (or its thenable could not be followed): its send is
 * failed unless answered, and the error kept for the turn. */
static void ta_napi_send_raised(napi_env env, ta_napi_view *view) {
    napi_value error = NULL;
    if (napi_get_and_clear_last_exception(env, &error) != napi_ok) error = NULL;
    ta_napi_send_fail_unanswered(env, view);
    if (error != NULL) napi_throw(env, error);
    ta_napi_keep_error(env, view->host);
}

/* A handler's promise rejected (bound to its view): the send is failed if
 * nobody answered it, and the rejection goes on, so the promise `then`
 * made rejects unhandled and the runtime reports it (decision d-errors). */
static napi_value ta_napi_send_rejected(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL};
    ta_napi_view view;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
    if (argc < 2) argv[1] = ta_napi_undefined(env);
    if (ta_napi_view_of(env, argv[0], &view)) ta_napi_send_fail_unanswered(env, &view);
    napi_value ignored;
    napi_get_and_clear_last_exception(env, &ignored);
    napi_throw(env, argv[1]);
    return NULL;
}

/* ---- dispatch ------------------------------------------------------------------------ */

/* A new view of effect `index`: its class (send or entry), its fields'
 * slots holes until read. */
typedef struct ta_napi_view_spec {
    ta_napi_host *host;
    napi_ref class;
    uint32_t index, processor, fields;
} ta_napi_view_spec;

static bool ta_napi_view_new(napi_env env, const ta_napi_view_spec *spec, ta_napi_view *view) {
    ta_napi_host *host = spec->host;
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value class, keys, argv[2], acks, index, processor, flags;
    memset(view, 0, sizeof *view);
    view->host = host;
    view->wrapper = ta_napi_self(env, host);
    view->acks = (double)host->acks;
    view->index = spec->index;
    view->processor = spec->processor;
    if (instance == NULL || view->wrapper == NULL || napi_get_reference_value(env, spec->class, &class) != napi_ok ||
        napi_get_reference_value(env, instance->keys, &keys) != napi_ok ||
        napi_create_array_with_length(env, TA_NAPI_VIEW_FIELDS + spec->fields, &view->state) != napi_ok ||
        napi_create_double(env, view->acks, &acks) != napi_ok || napi_create_uint32(env, spec->index, &index) != napi_ok ||
        napi_create_uint32(env, spec->processor, &processor) != napi_ok || napi_create_uint32(env, 0, &flags) != napi_ok ||
        napi_set_element(env, view->state, TA_NAPI_VIEW_HOST, view->wrapper) != napi_ok ||
        napi_set_element(env, view->state, TA_NAPI_VIEW_ACKS, acks) != napi_ok ||
        napi_set_element(env, view->state, TA_NAPI_VIEW_INDEX, index) != napi_ok ||
        napi_set_element(env, view->state, TA_NAPI_VIEW_PROCESSOR, processor) != napi_ok ||
        napi_set_element(env, view->state, TA_NAPI_VIEW_FLAGS, flags) != napi_ok)
        return false;
    argv[0] = keys;
    argv[1] = view->state;
    return napi_new_instance(env, class, 2, argv, &view->self) == napi_ok;
}

/* The constructor of Send and LogEntry: only the drain makes one (it
 * passes the instance's keys, which no export reaches, and the state). */
static napi_value ta_napi_view_construct(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL}, self = NULL, keys, key;
    ta_napi_instance *instance = ta_napi_instance_of(env);
    bool same = false;
    if (instance == NULL || napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok ||
        napi_get_reference_value(env, instance->keys, &keys) != napi_ok)
        return NULL;
    if (argc < 2 || napi_strict_equals(env, argv[0], keys, &same) != napi_ok || !same) {
        napi_throw_type_error(env, NULL, "a view of an effect is made by the host and given to its handler");
        return NULL;
    }
    if (napi_get_named_property(env, keys, "view", &key) != napi_ok) return NULL;
    napi_property_descriptor property = {NULL, key, NULL, NULL, NULL, argv[1], napi_default, NULL};
    if (napi_define_properties(env, self, 1, &property) != napi_ok) return NULL;
    return self;
}

/* Every field read into its slot, then the view marked a record. */
static bool ta_napi_send_materialize(napi_env env, ta_napi_view *view) {
    for (uint32_t field = 0; field < TA_NAPI_SEND_COUNT; field++)
        if (ta_napi_send_read(env, view, field) == NULL) return false;
    return ta_napi_view_flag(env, view, TA_NAPI_VIEW_RECORD);
}

/* A handler returned a thenable (decision d-async): the view is filled
 * before the ack, and its rejection is followed with the rejection
 * function bound to the view. False with an exception pending (reading
 * `then` threw, a field, or `then` itself). */
static bool ta_napi_send_follow(napi_env env, ta_napi_view *view, napi_value result) {
    napi_value then;
    if (!ta_napi_is(env, result, napi_object) && !ta_napi_is(env, result, napi_function)) return true;
    if (napi_get_named_property(env, result, "then", &then) != napi_ok) return false;
    if (!ta_napi_is(env, then, napi_function)) return true; /* not a thenable: nothing outlives the ack */
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value rejected, bind, bound, argv[2];
    if (instance == NULL || !ta_napi_send_materialize(env, view) ||
        napi_get_reference_value(env, instance->rejected, &rejected) != napi_ok ||
        napi_get_named_property(env, rejected, "bind", &bind) != napi_ok)
        return false;
    argv[0] = ta_napi_undefined(env);
    argv[1] = view->self;
    if (napi_call_function(env, rejected, bind, 2, argv, &bound) != napi_ok) return false;
    argv[1] = bound;
    napi_value ignored;
    return napi_call_function(env, result, then, 2, argv, &ignored) == napi_ok;
}

/* A SEND to an I/O processor (IO_PROCESSOR i + 1 for the config's
 * ioProcessors[i]) or to the transport (0, another system's address): its
 * handler gets the host and a thin view; without a handler the send is
 * failed. A send to an invoked service (TA_NIL) waits for the services
 * step and is dropped until then. */
static void ta_napi_dispatch_send(napi_env env, ta_napi_host *host, uint32_t index) {
    uint32_t processor = (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_IO_PROCESSOR);
    if (processor == TA_NIL || processor > host->processors) return;
    ta_napi_instance *instance = ta_napi_instance_of(env);
    ta_napi_view view;
    if (instance == NULL || !ta_napi_view_new(env, &(ta_napi_view_spec){host, instance->send, index, processor, TA_NAPI_SEND_COUNT}, &view)) {
        ta_napi_keep_error(env, host);
        return;
    }
    napi_value handler = ta_napi_handler_of(env, view.wrapper, &(ta_napi_handler_at){"processors", processor});
    if (handler == NULL) {
        ta_napi_keep_error(env, host);
        return;
    }
    if (!ta_napi_is(env, handler, napi_function)) { /* no handler: a failed completion */
        ta_napi_answer answer = {NULL, ta_napi_undefined(env), NULL, NULL, NULL};
        if (!ta_napi_send_reply(env, &view, &answer)) ta_napi_keep_error(env, host);
        return;
    }
    napi_value argv[2] = {view.wrapper, view.self}, result;
    if (napi_call_function(env, ta_napi_undefined(env), handler, 2, argv, &result) != napi_ok ||
        !ta_napi_send_follow(env, &view, result))
        ta_napi_send_raised(env, &view);
}

/* A LOG: the log handler gets the host and a thin LogEntry; without one
 * the line is discarded. */
static void ta_napi_dispatch_log(napi_env env, ta_napi_host *host, uint32_t index) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value wrapper = ta_napi_self(env, host), handler = NULL, result;
    if (instance == NULL || wrapper == NULL) return;
    handler = ta_napi_handler_of(env, wrapper, &(ta_napi_handler_at){"log", UINT32_MAX});
    if (handler == NULL) {
        ta_napi_keep_error(env, host);
        return;
    }
    if (!ta_napi_is(env, handler, napi_function)) return;
    ta_napi_view view;
    if (!ta_napi_view_new(env, &(ta_napi_view_spec){host, instance->entry, index, 0, TA_NAPI_ENTRY_COUNT}, &view)) {
        ta_napi_keep_error(env, host);
        return;
    }
    napi_value argv[2] = {wrapper, view.self};
    if (napi_call_function(env, ta_napi_undefined(env), handler, 2, argv, &result) != napi_ok) ta_napi_keep_error(env, host);
}

void ta_napi_take(napi_env env, ta_napi_host *host, bool traces) {
    ta_bytes taken; /* the host's view, until the ack */
    ta_status status = traces ? ta_host_traces(host->host, &taken) : ta_host_logs(host->host, &taken);
    uint8_t bit = traces ? 1u : 2u;
    if (status == TA_OK) {
        host->retake &= (uint8_t)~bit;
        return; /* no telemetry handler yet (p2): dropped, so the encoder goes on */
    }
    if (status == TA_ERROR_NO_MEMORY && !(host->retake & bit)) {
        host->retake |= bit;
        ta_napi_throw(env, &(ta_napi_refusal){status, traces ? "the take of traces was refused"
                                                             : "the take of logs was refused"});
        ta_napi_keep_error(env, host);
    }
}

void ta_napi_dispatch(napi_env env, const ta_napi_batch *batch, uint32_t index) {
    ta_napi_drain_state *state = batch->state;
    ta_napi_host *host = state->host;
    napi_escapable_handle_scope scope;
    if (napi_open_escapable_handle_scope(env, &scope) != napi_ok) return; /* one per effect: a turn is one native call */
    switch (batch->effects[index].kind) {
    case TA_EFFECT_SEND: ta_napi_dispatch_send(env, host, index); break;
    case TA_EFFECT_LOG: ta_napi_dispatch_log(env, host, index); break;
    case TA_EFFECT_REQUEST: {
        /* a fetch at run time: refused in C until it is built (AGENTS.md,
         * "Layers and the resolver"); the requester gets error.execution */
        ta_resume resume = {(uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_SEGMENT),
                            (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_TOKEN), TA_ERROR_INVALID, 0, 0};
        ta_host_resume(host->host, &resume);
        ta_napi_schedule(env, host); /* an input: the turn runs it */
        break;
    }
    case TA_HOST_EFFECT_REPLY: { /* the creation's host.config; the typed methods' with p1-methods */
        napi_value reply = ta_napi_value_read(env, ta_host_effect_bytes(host->host, index, TA_EFFECT_FIELD_WIRE));
        if (reply == NULL) ta_napi_keep_error(env, host);
        else if (napi_escape_handle(env, scope, reply, &state->reply) != napi_ok) state->reply = NULL;
        break;
    }
    case TA_HOST_EFFECT_TRACES: ta_napi_take(env, host, true); break;
    case TA_HOST_EFFECT_LOGS: ta_napi_take(env, host, false); break;
    default: /* DONE, ERROR: no application callback; SEGMENT_FREED: the host keeps the images, the
                addon none; a NOTIFICATION it does not know (none is defined); the rest has no
                API yet */
        break;
    }
    napi_close_escapable_handle_scope(env, scope);
}

napi_status ta_napi_effects_define(napi_env env, napi_value exports) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    (void)exports;
    if (instance == NULL) return napi_pending_exception;
    napi_property_attributes getter = napi_default_jsproperty & ~napi_writable;
    napi_property_descriptor send[] = {
        {"session", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_SEND, TA_NAPI_SEND_SESSION)},
        {"event", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_SEND, TA_NAPI_SEND_EVENT)},
        {"target", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_SEND, TA_NAPI_SEND_TARGET)},
        {"sendid", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_SEND, TA_NAPI_SEND_SENDID)},
        {"data", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_SEND, TA_NAPI_SEND_DATA)},
        {"type", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_SEND, TA_NAPI_SEND_TYPE)},
        {"origin", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_SEND, TA_NAPI_SEND_ORIGIN)},
        {"traceparent", NULL, NULL, ta_napi_view_get, NULL, NULL, getter,
         TA_NAPI_GETTER(TA_NAPI_KIND_SEND, TA_NAPI_SEND_TRACEPARENT)},
        {"message", NULL, NULL, ta_napi_send_message, NULL, NULL, getter, NULL},
        {"answer", NULL, ta_napi_send_answer, NULL, NULL, NULL, napi_default_method, NULL},
        {"fail", NULL, ta_napi_send_fail, NULL, NULL, NULL, napi_default_method, NULL},
    };
    napi_property_descriptor entry[] = {
        {"session", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_ENTRY, TA_NAPI_ENTRY_SESSION)},
        {"label", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_ENTRY, TA_NAPI_ENTRY_LABEL)},
        {"text", NULL, NULL, ta_napi_view_get, NULL, NULL, getter, TA_NAPI_GETTER(TA_NAPI_KIND_ENTRY, TA_NAPI_ENTRY_TEXT)},
    };
    napi_value send_class, entry_class, rejected, keys, description, symbol;
    napi_status status = napi_define_class(env, "Send", NAPI_AUTO_LENGTH, ta_napi_view_construct, NULL,
                                           sizeof send / sizeof send[0], send, &send_class);
    if (status == napi_ok) status = napi_create_reference(env, send_class, 1, &instance->send);
    if (status == napi_ok)
        status = napi_define_class(env, "LogEntry", NAPI_AUTO_LENGTH, ta_napi_view_construct, NULL,
                                   sizeof entry / sizeof entry[0], entry, &entry_class);
    if (status == napi_ok) status = napi_create_reference(env, entry_class, 1, &instance->entry);
    if (status == napi_ok)
        status = napi_create_function(env, "rejected", NAPI_AUTO_LENGTH, ta_napi_send_rejected, NULL, &rejected);
    if (status == napi_ok) status = napi_create_reference(env, rejected, 1, &instance->rejected);
    if (status == napi_ok) status = napi_get_reference_value(env, instance->keys, &keys);
    if (status == napi_ok) status = napi_create_string_utf8(env, "tinyactors.view", NAPI_AUTO_LENGTH, &description);
    if (status == napi_ok) status = napi_create_symbol(env, description, &symbol);
    if (status == napi_ok) status = napi_set_named_property(env, keys, "view", symbol);
    return status;
}
