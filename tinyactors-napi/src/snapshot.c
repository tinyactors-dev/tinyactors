/* snapshot.c: readSnapshot(bytes), a snapshot read without a host, over
 * core's snapshot reader (tinyactors-core/docs/snapshot-requests.md).
 *
 * The reader is one Buffer: ta_host_snapshot_reader_open copies the pack
 * and reads each live segment's region into it, and it holds pointers into
 * itself, so it is kept under the instance's `snapshot` symbol on the
 * object and never handed out (napi_wrap of its start, no finalizer: the
 * Buffer is the memory, gone with the object; a Buffer's bytes never
 * move). The pack is copied 8-aligned into the environment's arena for the
 * open only. The object's one method is request(method, params): the
 * reader's request, params written with the value table, the reply read
 * with it; the convenience layer's Snapshot mirrors core's readers over it.
 * A request's working memory and its reply are the environment's arena, a
 * mark per call. */
#include "addon.h"
#include <string.h>

/* napi_type_tag_object's tag of a snapshot reader. */
static const napi_type_tag ta_napi_snapshot_tag = {0x7461736e61707368ULL, 0x6f74417069303031ULL};

/* A reply's room on the first try; a larger one is asked for again with
 * the length the reader gave. */
static const size_t ta_napi_snapshot_reply_room = 16384;

static napi_value ta_napi_snapshot_symbol(napi_env env) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value keys, key;
    if (instance == NULL || napi_get_reference_value(env, instance->keys, &keys) != napi_ok ||
        napi_get_named_property(env, keys, "snapshot", &key) != napi_ok)
        return NULL;
    return key;
}

static napi_value ta_napi_text_value(napi_env env, ta_string_view text) {
    napi_value value = NULL;
    napi_create_string_utf8(env, text.pointer ? text.pointer : "", text.length, &value);
    return value;
}

/* What readSnapshot read: the reader's Buffer and its start, the pack's header. */
typedef struct ta_napi_snapshot_made {
    napi_value block;
    const ta_host_snapshot_reader *reader;
    ta_host_snapshot_contents contents;
} ta_napi_snapshot_made;

/* The object: type-tagged, the header as properties, the Buffer under the
 * symbol, the reader wrapped last. */
static napi_value ta_napi_snapshot_object(napi_env env, const ta_napi_snapshot_made *made) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value class, self, external, key = ta_napi_snapshot_symbol(env), state, time = NULL, segments = NULL;
    void *data = NULL;
    const ta_host_snapshot_contents *contents = &made->contents;
    if (instance == NULL || key == NULL || napi_get_reference_value(env, instance->snapshot, &class) != napi_ok ||
        napi_create_external(env, (void *)(uintptr_t)made->reader, NULL, NULL, &external) != napi_ok ||
        napi_new_instance(env, class, 1, &external, &self) != napi_ok ||
        napi_create_buffer_copy(env, contents->state_length, contents->state_length ? contents->state : "", &data, &state) !=
            napi_ok ||
        napi_create_bigint_uint64(env, contents->time, &time) != napi_ok ||
        napi_create_uint32(env, contents->image_count, &segments) != napi_ok)
        return NULL;
    napi_property_descriptor properties[] = {
        {"systemID", NULL, NULL, NULL, NULL, ta_napi_text_value(env, contents->system_id), napi_enumerable, NULL},
        {"datamodel", NULL, NULL, NULL, NULL, ta_napi_text_value(env, contents->datamodel), napi_enumerable, NULL},
        {"time", NULL, NULL, NULL, NULL, time, napi_enumerable, NULL},
        {"segments", NULL, NULL, NULL, NULL, segments, napi_enumerable, NULL},
        {"state", NULL, NULL, NULL, NULL, state, napi_enumerable, NULL},
        {NULL, key, NULL, NULL, NULL, made->block, napi_default, NULL},
    };
    if (napi_define_properties(env, self, sizeof properties / sizeof properties[0], properties) != napi_ok ||
        napi_wrap(env, self, (void *)(uintptr_t)made->reader, NULL, NULL, NULL) != napi_ok)
        return NULL;
    return self;
}

/* The reader in a Buffer of its size, made from the 8-aligned pack. */
static napi_value ta_napi_snapshot_open(napi_env env, ta_bytes pack) {
    ta_napi_snapshot_made made;
    memset(&made, 0, sizeof made);
    made.contents.size = sizeof made.contents;
    size_t size = ta_host_snapshot_reader_size(pack);
    if (!size || ta_host_snapshot_unpack(pack, &made.contents) != TA_OK)
        return ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_INVALID,
                                                     "not a snapshot of this build (another format or layout, or damaged)"});
    void *data = NULL;
    if (napi_create_buffer(env, size + 8u, &data, &made.block) != napi_ok) return NULL;
    unsigned char *start = (unsigned char *)data + ((8u - ((uintptr_t)data & 7u)) & 7u);
    ta_host_snapshot_reader_input input = {sizeof input, 0, pack, {start, size, 0}};
    ta_status status = ta_host_snapshot_reader_open(&input, &made.reader);
    if (status != TA_OK)
        return ta_napi_throw(env, &(ta_napi_refusal){status, "a segment of the snapshot is not this build's, or is damaged"});
    /* the header's views are into the caller's copy: the object's properties are made from them now */
    return ta_napi_snapshot_object(env, &made);
}

/* readSnapshot(bytes): a snapshot reader of a pack (host.snapshot()'s
 * bytes) of this build, read during the call and copied. */
static napi_value ta_napi_read_snapshot(napi_env env, napi_callback_info info) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (instance == NULL) return NULL;
    size_t argc = 1;
    napi_value argument = NULL;
    if (napi_get_cb_info(env, info, &argc, &argument, NULL, NULL) != napi_ok) return NULL;
    ta_napi_bytes bytes = {"readSnapshot takes a Uint8Array or an ArrayBuffer", {NULL, 0}};
    if (!ta_napi_bytes_of(env, argc < 1 ? NULL : argument, &bytes)) return NULL;
    ta_growable_arena_mark mark = ta_growable_arena_position(&instance->arena);
    void *pack = ta_growable_arena_alloc(&instance->arena, bytes.view.length ? bytes.view.length : 8u, 8);
    napi_value result = NULL;
    if (pack == NULL) {
        ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_NO_MEMORY, "no memory to read the snapshot"});
    } else {
        if (bytes.view.length) memcpy(pack, bytes.view.pointer, bytes.view.length);
        result = ta_napi_snapshot_open(env, (ta_bytes){pack, bytes.view.length});
    }
    ta_napi_arena_release(&instance->arena, mark, TA_NAPI_INSTANCE_ARENA_CAP);
    return result;
}

/* ---- request ------------------------------------------------------------------ */

/* What request was given: a method's name and its params (undefined: none). */
typedef struct ta_napi_snapshot_call {
    napi_value method, params;
} ta_napi_snapshot_call;

/* {"id": 1, "method": method, "params": params} into `writer`; false with
 * an exception thrown. */
static bool ta_napi_snapshot_message(napi_env env, const ta_napi_snapshot_call *call, ta_cbor_writer *writer) {
    napi_value method = call->method, params = call->params;
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, method, &type) != napi_ok) return false;
    if (type != napi_string) {
        napi_throw_type_error(env, NULL, "request takes a method's name: a string");
        return false;
    }
    ta_napi_text text = {&instance->arena, "the method's name", {NULL, 0}};
    if (!ta_napi_text_of(env, method, &text)) return false;
    bool given = napi_typeof(env, params, &type) == napi_ok && type != napi_undefined && type != napi_null;
    ta_cbor_write_map(writer, given ? 3 : 2);
    ta_cbor_write_text(writer, ta_string("id"));
    ta_cbor_write_uint(writer, 1);
    ta_cbor_write_text(writer, ta_string("method"));
    ta_cbor_write_text(writer, text.view);
    if (!given) return true;
    ta_cbor_write_text(writer, ta_string("params"));
    return ta_napi_value_write(env, params, &(ta_napi_writing){writer, &instance->arena, false, NULL, false});
}

/* The reply's result, or NULL with its error thrown as a TinyactorsError
 * (code, reason, path, message). */
static napi_value ta_napi_snapshot_result(napi_env env, ta_bytes reply) {
    napi_value decoded = ta_napi_value_read(env, reply), result = NULL, error = NULL;
    bool has = false;
    if (decoded == NULL) return NULL;
    if (napi_has_named_property(env, decoded, "result", &has) != napi_ok) return NULL;
    if (has) return napi_get_named_property(env, decoded, "result", &result) == napi_ok ? result : NULL;
    if (napi_get_named_property(env, decoded, "error", &error) != napi_ok) return NULL;
    return ta_napi_throw_core_error(env, &(ta_napi_core_error){error, TA_OK});
}

/* A request to a reader, its reply in room of `arena`. */
typedef struct ta_napi_snapshot_asking {
    ta_growable_arena *arena;
    const ta_host_snapshot_reader *reader;
    const ta_host_snapshot_request *request;
} ta_napi_snapshot_asking;

/* The reply: a first try in `reply`'s capacity, then once more in the room
 * the reply needs. */
static ta_status ta_napi_snapshot_ask(const ta_napi_snapshot_asking *asking, ta_buffer *reply) {
    reply->pointer = ta_growable_arena_alloc(asking->arena, reply->capacity, 8);
    if (reply->pointer == NULL) return TA_ERROR_NO_MEMORY;
    ta_status status = ta_host_snapshot_reader_request(asking->reader, asking->request, reply);
    if (status != TA_ERROR_NO_MEMORY || reply->length <= reply->capacity) return status;
    reply->capacity = reply->length;
    reply->pointer = ta_growable_arena_alloc(asking->arena, reply->capacity, 8);
    if (reply->pointer == NULL) return TA_ERROR_NO_MEMORY;
    return ta_host_snapshot_reader_request(asking->reader, asking->request, reply);
}

/* request(method, params?): the reader's answer to a snapshot request. */
static napi_value ta_napi_snapshot_request(napi_env env, napi_callback_info info) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (instance == NULL) return NULL;
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL}, self;
    bool tagged = false;
    napi_valuetype type = napi_undefined;
    void *data = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok) return NULL;
    if (napi_typeof(env, self, &type) != napi_ok || type != napi_object ||
        napi_check_object_type_tag(env, self, &ta_napi_snapshot_tag, &tagged) != napi_ok || !tagged ||
        napi_unwrap(env, self, &data) != napi_ok) {
        napi_throw_type_error(env, NULL, "not a Snapshot");
        return NULL;
    }
    if (argc < 2 && napi_get_undefined(env, &argv[1]) != napi_ok) return NULL;
    if (argc < 1 && napi_get_undefined(env, &argv[0]) != napi_ok) return NULL;
    const ta_host_snapshot_reader *reader = (const ta_host_snapshot_reader *)data;
    ta_growable_arena_mark mark = ta_growable_arena_position(&instance->arena);
    ta_allocator scratch = ta_growable_arena_allocator(&instance->arena);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, &scratch);
    napi_value result = NULL;
    ta_bytes message;
    if (ta_napi_snapshot_message(env, &(ta_napi_snapshot_call){argv[0], argv[1]}, &writer)) {
        ta_status status = ta_cbor_writer_bytes(&writer, &message);
        ta_host_snapshot_request request = {sizeof request, 0, message, &scratch};
        ta_buffer reply = {NULL, ta_napi_snapshot_reply_room, 0};
        if (status == TA_OK)
            status = ta_napi_snapshot_ask(&(ta_napi_snapshot_asking){&instance->arena, reader, &request}, &reply);
        if (status != TA_OK) ta_napi_throw(env, &(ta_napi_refusal){status, "the snapshot request could not be answered"});
        else result = ta_napi_snapshot_result(env, (ta_bytes){reply.pointer, reply.length});
    }
    ta_cbor_writer_free(&writer);
    ta_napi_arena_release(&instance->arena, mark, TA_NAPI_INSTANCE_ARENA_CAP);
    return result;
}

/* The constructor, called by readSnapshot only, with the reader as an
 * external: type-tagged; readSnapshot wraps it. */
static napi_value ta_napi_snapshot_new(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argument = NULL, self;
    napi_valuetype type = napi_undefined;
    if (napi_get_cb_info(env, info, &argc, &argument, &self, NULL) != napi_ok) return NULL;
    if (argc < 1 || napi_typeof(env, argument, &type) != napi_ok || type != napi_external) {
        napi_throw_type_error(env, NULL, "a Snapshot is made by readSnapshot");
        return NULL;
    }
    if (napi_type_tag_object(env, self, &ta_napi_snapshot_tag) != napi_ok) return NULL;
    return self;
}

napi_status ta_napi_snapshot_define(napi_env env, napi_value exports) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (instance == NULL) return napi_pending_exception;
    napi_value keys, description, symbol, class = NULL;
    napi_status status = napi_get_reference_value(env, instance->keys, &keys);
    if (status == napi_ok) status = napi_create_string_utf8(env, "tinyactors.snapshot", NAPI_AUTO_LENGTH, &description);
    if (status == napi_ok) status = napi_create_symbol(env, description, &symbol);
    if (status == napi_ok) status = napi_set_named_property(env, keys, "snapshot", symbol);
    napi_property_descriptor properties[] = {
        {"request", NULL, ta_napi_snapshot_request, NULL, NULL, NULL, napi_default_method, NULL},
    };
    if (status == napi_ok)
        status = napi_define_class(env, "SnapshotReader", NAPI_AUTO_LENGTH, ta_napi_snapshot_new, NULL,
                                   sizeof properties / sizeof properties[0], properties, &class);
    if (status == napi_ok) status = napi_create_reference(env, class, 1, &instance->snapshot);
    napi_property_descriptor exported[] = {
        {"readSnapshot", NULL, ta_napi_read_snapshot, NULL, NULL, NULL, napi_enumerable, NULL},
    };
    if (status == napi_ok) status = napi_define_properties(env, exports, 1, exported);
    return status;
}
