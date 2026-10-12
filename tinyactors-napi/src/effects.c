/* effects.c — one record per effect, read with the accessors
 * (ta_host_effect_number, _text, _bytes; bindings.md, section 6), handed to
 * the application's handler for its kind (`on` in the Host's options). What
 * each kind carries is system.h's and host.h's field use: LOG's auxiliary
 * is the label and its event the text; DONE's target is the actor's name,
 * its payload the donedata and its auxiliary the <final> that ended it;
 * ERROR's flags are the fault code; PEER and PEER_ERROR are written as
 * envelope bytes (ta_host_envelope). */
#include "addon.h"

#include <string.h>

/* One record being built: the host, the effect's index and the object. */
typedef struct ta_napi_record {
    ta_napi_host *host;
    uint32_t index;
    napi_value object;
} ta_napi_record;

/* A text field of the effect, as the record's member `name`. */
typedef struct ta_napi_text_field {
    const char *name;
    uint32_t field; /* TA_EFFECT_FIELD_* */
} ta_napi_text_field;

static void ta_napi_record_text(const ta_napi_record *record, const ta_napi_text_field *text_field) {
    napi_env env = record->host->env;
    ta_string_view text = ta_host_effect_text(record->host->host, record->index, text_field->field);
    napi_value value;
    if (napi_create_string_utf8(env, text.pointer ? text.pointer : "", text.length, &value) == napi_ok)
        napi_set_named_property(env, record->object, text_field->name, value);
}

static void ta_napi_record_session(const ta_napi_record *record) {
    napi_env env = record->host->env;
    napi_value value;
    uint64_t session = ta_host_effect_number(record->host->host, record->index, TA_EFFECT_FIELD_SESSION);
    if (napi_create_bigint_uint64(env, session, &value) == napi_ok)
        napi_set_named_property(env, record->object, "session", value);
}

/* The payload's value, through its datamodel's to_wire and the value table. */
static void ta_napi_record_data(const ta_napi_record *record, const char *name) {
    ta_napi_host *host = record->host;
    ta_napi_payload_read read = {
        (uint32_t)ta_host_effect_number(host->host, record->index, TA_EFFECT_FIELD_DATAMODEL),
        ta_host_effect_number(host->host, record->index, TA_EFFECT_FIELD_PAYLOAD),
    };
    napi_value value;
    ta_status status = ta_napi_values_of_payload(host, &read, &value);
    if (status != TA_OK) {
        ta_napi_throw_status(host->env, "ta_host_effects", status);
        return;
    }
    napi_set_named_property(host->env, record->object, name, value);
}

/* A PEER or PEER_ERROR effect as envelope bytes (ta_host_envelope: a view
 * of the host's memory until the next call into it), copied into a
 * Uint8Array at once. */
static void ta_napi_record_envelope(const ta_napi_record *record) {
    ta_napi_host *host = record->host;
    napi_env env = host->env;
    const ta_host_effect *effects = NULL;
    ta_host_effects(host->host, &effects);
    ta_bytes envelope;
    ta_status status = ta_host_envelope(host->host, &effects[record->index], &envelope);
    napi_value value;
    if (status == TA_OK && ta_napi_values_uint8array(env, envelope, &value) == napi_ok)
        napi_set_named_property(env, record->object, "envelope", value);
    else
        ta_napi_throw_status(env, "ta_host_envelope", status);
    uint64_t peer = ta_host_effect_number(host->host, record->index, TA_EFFECT_FIELD_PEER);
    if (napi_create_bigint_uint64(env, peer, &value) == napi_ok) napi_set_named_property(env, record->object, "peer", value);
}

/* A retired segment: its image's block is freed (bindings.md, section 4). */
static void ta_napi_segment_freed(ta_napi_host *host, uint32_t index) {
    ta_napi_image_free(host, (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_SEGMENT));
}

/* The handler on[name], called with the record; nothing when there is none. */
static void ta_napi_record_hand(const ta_napi_record *record, const char *name) {
    napi_env env = record->host->env;
    napi_value handlers, handler, undefined, result;
    napi_valuetype type = napi_undefined;
    if (ta_napi_exception_pending(env)) return;
    if (!record->host->handlers || napi_get_reference_value(env, record->host->handlers, &handlers) != napi_ok ||
        napi_get_named_property(env, handlers, name, &handler) != napi_ok ||
        napi_typeof(env, handler, &type) != napi_ok || type != napi_function)
        return;
    napi_get_undefined(env, &undefined);
    napi_call_function(env, undefined, handler, 1, &record->object, &result);
}

/* The handler of a kind, or NULL when the application gave none: the record
 * is then not built. */
static int ta_napi_has_handler(ta_napi_host *host, const char *name) {
    napi_env env = host->env;
    napi_value handlers, handler;
    napi_valuetype type = napi_undefined;
    return host->handlers && napi_get_reference_value(env, host->handlers, &handlers) == napi_ok &&
           napi_get_named_property(env, handlers, name, &handler) == napi_ok &&
           napi_typeof(env, handler, &type) == napi_ok && type == napi_function;
}

void ta_napi_effect_handle(ta_napi_host *host, uint32_t index) {
    napi_env env = host->env;
    uint32_t kind = (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_KIND);
    const char *name = NULL;
    switch (kind) {
    case TA_EFFECT_LOG: name = "log"; break;
    case TA_EFFECT_DONE: name = "done"; break;
    case TA_EFFECT_ERROR: name = "error"; break;
    case TA_HOST_EFFECT_PEER: name = "peer"; break;
    case TA_HOST_EFFECT_PEER_ERROR: name = "peerError"; break;
    case TA_HOST_EFFECT_SEGMENT_FREED: ta_napi_segment_freed(host, index); return;
    default: return; /* the other effects are later milestones' (ROADMAP.md) */
    }
    if (!ta_napi_has_handler(host, name)) return;
    napi_handle_scope scope;
    if (napi_open_handle_scope(env, &scope) != napi_ok) return;
    ta_napi_record record = {host, index, NULL};
    if (napi_create_object(env, &record.object) == napi_ok) {
        switch (kind) {
        case TA_EFFECT_LOG:
            ta_napi_record_session(&record);
            ta_napi_record_text(&record, &(ta_napi_text_field){"label", TA_EFFECT_FIELD_AUXILIARY});
            ta_napi_record_text(&record, &(ta_napi_text_field){"text", TA_EFFECT_FIELD_EVENT});
            break;
        case TA_EFFECT_DONE:
            ta_napi_record_session(&record);
            ta_napi_record_text(&record, &(ta_napi_text_field){"name", TA_EFFECT_FIELD_TARGET});
            ta_napi_record_data(&record, "data");
            ta_napi_record_text(&record, &(ta_napi_text_field){"final", TA_EFFECT_FIELD_AUXILIARY});
            break;
        case TA_EFFECT_ERROR: {
            napi_value code;
            ta_napi_record_session(&record);
            napi_create_uint32(env, (uint32_t)ta_host_effect_number(host->host, index, TA_EFFECT_FIELD_FLAGS), &code);
            napi_set_named_property(env, record.object, "code", code);
            break;
        }
        default: ta_napi_record_envelope(&record); break;
        }
        ta_napi_record_hand(&record, name);
    }
    napi_close_handle_scope(env, scope);
}
