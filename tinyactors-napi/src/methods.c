/* The typed methods (AGENTS.md, "What reaches the application";
 * bindings.md, section 10): one per method of
 * tinyactors-core/docs/requests.md but actor.send (which is host.send),
 * each shaped method(params, onResult, onError?) and returning nothing.
 *
 * A call adds a row to the host's pending table (ta_table), keeps its
 * handlers on the wrapper (handlers.pending[2 * slot] and [2 * slot + 1],
 * under the instance's `handlers` symbol: never a strong napi_ref, so a
 * host whose handlers close over it is still collected), advances the
 * clocks and posts {"method", "params"?, "id"} with the row's handle as
 * the id: params, an object or undefined (none), passed as given, so core
 * checks them and answers in the reply. A refusal at the call
 * (ta_host_post's TA_ERROR_NO_MEMORY or TA_ERROR_STATE) removes the row
 * and throws at once; else a turn is scheduled.
 *
 * The drain that dispatches the REPLY, a later one, reads its id in C,
 * finds the row (a stale or unknown id is dropped: the generation refuses
 * it), removes it, decodes the reply once and calls onResult(host,
 * result), or onError(host, error) with a TinyactorsError {code, reason,
 * path, message}; without onError the error is kept for the turn. At
 * close the handlers object goes and the table is freed with the host:
 * pending handlers are dropped, never called. */
#include "addon.h"

/* A typed method: its name in requests.md. */
typedef struct ta_napi_method {
    const char *js;   /* the Host's method */
    const char *core; /* the posted method */
} ta_napi_method;

static const ta_napi_method ta_napi_methods[] = {
    {"actorState", "actor.state"},         {"actorContext", "actor.context"},     {"actorList", "actor.list"},
    {"actorGet", "actor.get"},             {"hostStats", "host.stats"},           {"hostConfig", "host.config"},
    {"traceConfigure", "trace.configure"}, {"traceWatch", "trace.watch"},         {"traceUnwatch", "trace.unwatch"},
    {"traceEnd", "trace.end"},             {"traceSpansDecode", "trace.spans.decode"},
};

#define TA_NAPI_METHODS (sizeof ta_napi_methods / sizeof ta_napi_methods[0])

static bool ta_napi_methods_is(napi_env env, napi_value value, napi_valuetype want) {
    napi_valuetype type = napi_undefined;
    return value != NULL && napi_typeof(env, value, &type) == napi_ok && type == want;
}

/* The wrapper's array of pending handlers, made on first use; NULL when
 * the host's handlers are gone (closed) or with an exception pending. */
static napi_value ta_napi_pending_list(napi_env env, napi_value wrapper) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value keys, key, handlers, list;
    if (instance == NULL || wrapper == NULL || napi_get_reference_value(env, instance->keys, &keys) != napi_ok ||
        napi_get_named_property(env, keys, "handlers", &key) != napi_ok ||
        napi_get_property(env, wrapper, key, &handlers) != napi_ok || !ta_napi_methods_is(env, handlers, napi_object) ||
        napi_get_named_property(env, handlers, "pending", &list) != napi_ok)
        return NULL;
    bool array = false;
    if (napi_is_array(env, list, &array) != napi_ok) return NULL;
    if (array) return list;
    if (napi_create_array(env, &list) != napi_ok || napi_set_named_property(env, handlers, "pending", list) != napi_ok)
        return NULL;
    return list;
}

/* A pending row's handlers on the wrapper: set (`handlers` two values) or
 * cleared (undefined). */
typedef struct ta_napi_pending_at {
    napi_value list;
    uint32_t slot;
} ta_napi_pending_at;

static bool ta_napi_pending_set(napi_env env, const ta_napi_pending_at *at, const napi_value handlers[2]) {
    return napi_set_element(env, at->list, 2 * at->slot, handlers[0]) == napi_ok &&
           napi_set_element(env, at->list, 2 * at->slot + 1, handlers[1]) == napi_ok;
}

static void ta_napi_pending_clear(napi_env env, const ta_napi_pending_at *at) {
    napi_value undefined[2];
    if (napi_get_undefined(env, &undefined[0]) != napi_ok) return;
    undefined[1] = undefined[0];
    ta_napi_pending_set(env, at, undefined);
}

/* The message {"method", "params"?, "id"} in the call's arena, posted. A
 * value that cannot be written leaves its exception pending (status
 * TA_OK, false); a refused post its status. */
typedef struct ta_napi_message {
    const ta_napi_method *method;
    napi_value params; /* undefined: none */
    uint64_t id;
} ta_napi_message;

typedef struct ta_napi_posting {
    ta_napi_host *host;
    ta_status status; /* out: the post's, or TA_OK when writing failed */
} ta_napi_posting;

static bool ta_napi_post(napi_env env, const ta_napi_message *message, ta_napi_posting *posting) {
    ta_napi_host *host = posting->host;
    bool with_params = !ta_napi_methods_is(env, message->params, napi_undefined);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, &host->arena_allocator);
    ta_cbor_write_map(&writer, with_params ? 3 : 2);
    ta_cbor_write_text(&writer, ta_string("method"));
    ta_cbor_write_text(&writer, ta_string(message->method->core));
    bool ok = true;
    if (with_params) {
        ta_cbor_write_text(&writer, ta_string("params"));
        ok = ta_napi_value_write(env, message->params, &(ta_napi_writing){&writer, &host->arena, false});
    }
    posting->status = TA_OK;
    if (ok) {
        ta_cbor_write_text(&writer, ta_string("id"));
        ta_cbor_write_uint(&writer, message->id);
        ta_bytes bytes;
        posting->status = ta_cbor_writer_bytes(&writer, &bytes);
        /* a nested call (a getter of params) may have closed the host: deferred, so it is still open */
        if (posting->status == TA_OK) posting->status = ta_host_post(host->host, bytes);
        ok = posting->status == TA_OK;
    }
    ta_cbor_writer_free(&writer);
    return ok;
}

/* host.<method>(params, onResult, onError?) */
static napi_value ta_napi_typed(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {NULL, NULL, NULL}, self = NULL, undefined, result = NULL;
    void *data = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, &self, &data) != napi_ok || napi_get_undefined(env, &undefined) != napi_ok)
        return NULL;
    for (size_t i = argc; i < 3; i++) argv[i] = undefined;
    const ta_napi_method *method = (const ta_napi_method *)data;
    ta_napi_call call;
    if (!ta_napi_enter(env, self, &call)) return NULL;
    ta_napi_host *host = call.host;
    napi_value handlers[2] = {argv[1], argv[2]};
    if (ta_napi_methods_is(env, handlers[1], napi_null)) handlers[1] = undefined;
    if (!ta_napi_methods_is(env, handlers[0], napi_function) ||
        !(ta_napi_methods_is(env, handlers[1], napi_function) || ta_napi_methods_is(env, handlers[1], napi_undefined))) {
        napi_throw_type_error(env, NULL, "a typed method takes (params, onResult(host, result), onError?(host, error))");
        ta_napi_leave(&call);
        return NULL;
    }
    napi_value list = ta_napi_pending_list(env, self);
    uint64_t id = 0;
    ta_napi_pending_row *row = list != NULL ? (ta_napi_pending_row *)ta_table_add(&host->pending, &id) : NULL;
    if (list == NULL) {
        bool pending = false;
        if (napi_is_exception_pending(env, &pending) == napi_ok && !pending)
            ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_STATE, "the Host's handlers are gone"});
    } else if (row == NULL) {
        ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_NO_MEMORY, "no memory for a pending method"});
    } else {
        ta_napi_pending_at at = {list, (uint32_t)(id & 0xFFFFFFFFu) - 1u}; /* the handle's slot + 1 */
        row->slot = at.slot;
        ta_napi_advance(env, host); /* trace.watch's end is a time of the host's clock */
        ta_napi_posting posting = {host, TA_OK};
        if (!ta_napi_pending_set(env, &at, handlers)) {
            ta_table_remove(&host->pending, id);
        } else if (!ta_napi_post(env, &(ta_napi_message){method, argv[0], id}, &posting)) {
            /* refused at the call: nothing was applied, no reply comes */
            ta_napi_pending_clear(env, &at);
            ta_table_remove(&host->pending, id);
            if (posting.status != TA_OK) ta_napi_throw(env, &(ta_napi_refusal){posting.status, "the method's post was refused"});
        } else {
            ta_napi_schedule(env, host);
            result = undefined;
        }
    }
    ta_napi_leave(&call);
    return result;
}

/* The id of a reply's WIRE, read in C: its first key is "id" (requests.md,
 * "The envelope"). False for an id that is not an unsigned integer (null:
 * the message could not be read; text: not ours). */
static bool ta_napi_reply_id(ta_bytes wire, uint64_t *id) {
    ta_cbor_reader reader;
    ta_cbor_item item;
    ta_cbor_reader_init(&reader, wire);
    if (ta_cbor_read(&reader, &item) != 1 || item.kind != TA_CBOR_MAP || item.count == 0) return false;
    if (ta_cbor_read(&reader, &item) != 1 || item.kind != TA_CBOR_TEXT || item.bytes.length != 2 ||
        ((const char *)item.bytes.pointer)[0] != 'i' || ((const char *)item.bytes.pointer)[1] != 'd')
        return false;
    if (ta_cbor_read(&reader, &item) != 1 || item.kind != TA_CBOR_UINT) return false;
    *id = item.value;
    return true;
}

/* A reply's `error` as a TinyactorsError (code, reason, path, message; no
 * status: it is no call's), or NULL with an exception pending. */
static napi_value ta_napi_reply_error(napi_env env, napi_value error) {
    napi_value made = NULL;
    ta_napi_throw_core_error(env, &(ta_napi_core_error){error, TA_OK});
    if (napi_get_and_clear_last_exception(env, &made) != napi_ok) return NULL;
    return made;
}

napi_value ta_napi_reply(napi_env env, ta_napi_host *host, uint32_t index) {
    napi_value undefined;
    if (napi_get_undefined(env, &undefined) != napi_ok) return NULL;
    ta_bytes wire = ta_host_effect_bytes(host->host, index, TA_EFFECT_FIELD_WIRE); /* the host's, until the ack */
    uint64_t id = 0;
    if (!ta_napi_reply_id(wire, &id)) return NULL; /* not a message of ours */
    if (id == 0) {                                 /* the creation's host.config: its caller reads it */
        napi_value reply = ta_napi_value_read(env, wire);
        if (reply == NULL) ta_napi_keep_error(env, host);
        return reply;
    }
    ta_napi_pending_row *row = (ta_napi_pending_row *)ta_table_get(&host->pending, id);
    if (row == NULL) return NULL; /* stale or unknown: dropped */
    ta_napi_pending_at at = {ta_napi_pending_list(env, ta_napi_self(env, host)), row->slot};
    ta_table_remove(&host->pending, id);
    napi_value handlers[2] = {NULL, NULL}, wrapper = ta_napi_self(env, host);
    if (at.list == NULL || wrapper == NULL || napi_get_element(env, at.list, 2 * at.slot, &handlers[0]) != napi_ok ||
        napi_get_element(env, at.list, 2 * at.slot + 1, &handlers[1]) != napi_ok)
        return NULL;
    ta_napi_pending_clear(env, &at);
    napi_value reply = ta_napi_value_read(env, wire), error = NULL, value = NULL, called;
    bool failed = false;
    if (reply == NULL || !ta_napi_methods_is(env, reply, napi_object) ||
        napi_has_named_property(env, reply, "error", &failed) != napi_ok) {
        ta_napi_keep_error(env, host);
        return NULL;
    }
    if (failed) {
        if (napi_get_named_property(env, reply, "error", &value) != napi_ok ||
            (error = ta_napi_reply_error(env, value)) == NULL) {
            ta_napi_keep_error(env, host);
            return NULL;
        }
        if (!ta_napi_methods_is(env, handlers[1], napi_function)) { /* no onError: the turn reports it */
            napi_throw(env, error);
            ta_napi_keep_error(env, host);
            return NULL;
        }
        napi_value argv[2] = {wrapper, error};
        if (napi_call_function(env, undefined, handlers[1], 2, argv, &called) != napi_ok) ta_napi_keep_error(env, host);
        return NULL;
    }
    if (napi_get_named_property(env, reply, "result", &value) != napi_ok) {
        ta_napi_keep_error(env, host);
        return NULL;
    }
    napi_value argv[2] = {wrapper, value};
    if (!ta_napi_methods_is(env, handlers[0], napi_function)) return NULL;
    if (napi_call_function(env, undefined, handlers[0], 2, argv, &called) != napi_ok) ta_napi_keep_error(env, host);
    return NULL;
}

napi_status ta_napi_methods_define(napi_env env, napi_value host_class) {
    napi_property_descriptor properties[TA_NAPI_METHODS];
    for (size_t i = 0; i < TA_NAPI_METHODS; i++)
        properties[i] = (napi_property_descriptor){ta_napi_methods[i].js, NULL, ta_napi_typed, NULL, NULL, NULL,
                                                   napi_default_method, (void *)&ta_napi_methods[i]};
    napi_value prototype;
    napi_status status = napi_get_named_property(env, host_class, "prototype", &prototype);
    if (status == napi_ok) status = napi_define_properties(env, prototype, TA_NAPI_METHODS, properties);
    return status;
}
