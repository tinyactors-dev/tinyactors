/* values.c — CBOR and JavaScript values, by the value table of
 * docs/values.md: one conversion for the API (config, requests, replies,
 * event data), the native datamodel's to_wire and from_wire, and the
 * package's `encode` and `decode` exports (ADR 0010, decision 4).
 *
 * Written: item by item with core's streaming writer (ta_cbor_writer),
 * into a room of the call's own. Read: item by item with core's reader
 * (ta_cbor_reader), straight into JavaScript values. Copied: a payload
 * between the API and the native datamodel goes by the same table without
 * CBOR (ta_napi_values_copy). The rules are those of core's ECMAScript
 * datamodel, so both of Node's datamodels read and write the same values
 * (tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c,
 * wire_write and wire_read): what JSON.stringify keeps, with integers,
 * BigInts and bytes exact; an actor reference (tag 32) read as its
 * address text and never written. */
#include "addon.h"

#include <string.h>

/* 2^53 - 1: the integers a number holds exactly. */
#define TA_NAPI_SAFE_INTEGER 9007199254740991.0
#define TA_NAPI_SAFE_UINT ((uint64_t)9007199254740991)

/* ---- rooms -------------------------------------------------------------------------- */

void ta_napi_room_init(ta_napi_room *room, napi_env env, ta_buffer stack) {
    room->env = env;
    room->allocator = ta_napi_allocator(env);
    room->pointer = room->stack = (char *)stack.pointer;
    room->capacity = stack.pointer ? stack.capacity : 0;
    room->length = 0;
}

int ta_napi_room_reserve(ta_napi_room *room, size_t more) {
    if (more <= room->capacity - room->length) return 1;
    if (more > SIZE_MAX / 4 - room->length) return 0;
    size_t need = room->length + more, capacity = room->capacity ? room->capacity : 256;
    while (capacity < need)
        capacity *= 2;
    char *pointer = (char *)room->allocator.alloc(room->allocator.context, capacity, 1);
    if (!pointer) return 0;
    if (room->length) memcpy(pointer, room->pointer, room->length);
    if (room->pointer != room->stack) room->allocator.free(room->allocator.context, room->pointer, room->capacity);
    room->pointer = pointer;
    room->capacity = capacity;
    return 1;
}

void ta_napi_room_free(ta_napi_room *room) {
    if (room->pointer && room->pointer != room->stack)
        room->allocator.free(room->allocator.context, room->pointer, room->capacity);
    room->pointer = room->stack = NULL;
    room->capacity = room->length = 0;
}

/* ---- strings ------------------------------------------------------------------------ */

/* Whether a string Node-API wrote U+FFFD into has a lone surrogate: its
 * UTF-16 read past the room's length, for the check only (Node-API writes
 * U+FFFD for one silently; bindings.md, section 9). */
static ta_status ta_napi_text_check(ta_napi_room *room, napi_value value) {
    size_t units = 0, read = 0;
    if (napi_get_value_string_utf16(room->env, value, NULL, 0, &units) != napi_ok) return TA_ERROR_INVALID;
    if (!ta_napi_room_reserve(room, (units + 1) * sizeof(char16_t) + 1)) return TA_ERROR_NO_MEMORY;
    size_t at = room->length + ((uintptr_t)(room->pointer + room->length) & 1); /* aligned for char16_t */
    char16_t *utf16 = (char16_t *)(void *)(room->pointer + at);
    if (napi_get_value_string_utf16(room->env, value, utf16, units + 1, &read) != napi_ok) return TA_ERROR_INVALID;
    for (size_t i = 0; i < read; i++) {
        uint32_t c = utf16[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < read && utf16[i + 1] >= 0xDC00 && utf16[i + 1] <= 0xDFFF) i++;
        else if (c >= 0xD800 && c <= 0xDFFF) return TA_ERROR_INVALID; /* a lone surrogate */
    }
    return TA_OK;
}

/* Whether `text` holds U+FFFD (EF BF BD). */
static int ta_napi_has_replacement(ta_bytes text) {
    const unsigned char *bytes = (const unsigned char *)text.pointer;
    for (size_t i = 0; i + 2 < text.length; i++)
        if (bytes[i] == 0xEF && bytes[i + 1] == 0xBF && bytes[i + 2] == 0xBD) return 1;
    return 0;
}

/* A string as UTF-8, appended to `room` (its length grows by `*length`):
 * the one copy Node-API forces. Written into the room's space; when less
 * than 4 bytes (the longest character) were left over, it may have
 * stopped short, so it is measured and written again. TA_ERROR_INVALID: a
 * lone surrogate, or not a string. */
static ta_status ta_napi_text_append(ta_napi_room *room, napi_value value, size_t *length) {
    size_t at = room->length, written = 0;
    if (!ta_napi_room_reserve(room, 64)) return TA_ERROR_NO_MEMORY;
    size_t space = room->capacity - at;
    if (napi_get_value_string_utf8(room->env, value, room->pointer + at, space, &written) != napi_ok)
        return TA_ERROR_INVALID;
    if (space - 1 - written < 4) {
        size_t need = 0;
        if (napi_get_value_string_utf8(room->env, value, NULL, 0, &need) != napi_ok) return TA_ERROR_INVALID;
        if (need > written) {
            if (!ta_napi_room_reserve(room, need + 1)) return TA_ERROR_NO_MEMORY;
            if (napi_get_value_string_utf8(room->env, value, room->pointer + at, need + 1, &written) != napi_ok)
                return TA_ERROR_INVALID;
        }
    }
    room->length = at + written;
    *length = written;
    if (ta_napi_has_replacement((ta_bytes){room->pointer + at, written})) return ta_napi_text_check(room, value);
    return TA_OK;
}

ta_status ta_napi_values_texts(ta_napi_room *room, const ta_napi_texts *texts) {
    size_t start = room->length;
    for (uint32_t i = 0; i < texts->count; i++) {
        napi_valuetype type = napi_undefined;
        size_t length = 0;
        texts->out[i].pointer = NULL;
        texts->out[i].length = 0;
        if (napi_typeof(room->env, texts->values[i], &type) != napi_ok) return TA_ERROR_INVALID;
        if (type == napi_undefined) continue;
        if (type != napi_string) return TA_ERROR_INVALID;
        ta_status status = ta_napi_text_append(room, texts->values[i], &length);
        if (status != TA_OK) return status;
        if (length > UINT32_MAX) return TA_ERROR_INVALID;
        texts->out[i].length = (uint32_t)length;
    }
    /* the views, once the room no longer moves: the texts lie end to end */
    for (uint32_t i = 0; i < texts->count; i++) {
        if (!texts->out[i].length) continue;
        texts->out[i].pointer = room->pointer + start;
        start += texts->out[i].length;
    }
    return TA_OK;
}

ta_status ta_napi_values_hold_text(napi_env env, napi_value value, ta_napi_held_text *out) {
    napi_valuetype type = napi_undefined;
    size_t length = 0, written = 0;
    void *data = NULL;
    if (napi_typeof(env, value, &type) != napi_ok || type != napi_string ||
        napi_get_value_string_utf8(env, value, NULL, 0, &length) != napi_ok || length > UINT32_MAX)
        return TA_ERROR_INVALID;
    if (napi_create_arraybuffer(env, length + 1, &data, &out->holder) != napi_ok) return TA_ERROR_NO_MEMORY;
    if (napi_get_value_string_utf8(env, value, (char *)data, length + 1, &written) != napi_ok) return TA_ERROR_INVALID;
    out->text.pointer = (const char *)data;
    out->text.length = (uint32_t)written;
    if (!ta_napi_has_replacement((ta_bytes){data, written})) return TA_OK;
    char stack[256];
    ta_napi_room room;
    ta_napi_room_init(&room, env, (ta_buffer){stack, sizeof stack, 0});
    ta_status status = ta_napi_text_check(&room, value);
    ta_napi_room_free(&room);
    return status;
}

/* ---- what both directions share ------------------------------------------------------- */

int ta_napi_values_bytes(napi_env env, napi_value value, ta_bytes *out) {
    bool is = false;
    void *data = NULL;
    size_t length = 0;
    out->pointer = NULL;
    out->length = 0;
    if (napi_is_arraybuffer(env, value, &is) == napi_ok && is) {
        if (napi_get_arraybuffer_info(env, value, &data, &length) != napi_ok) return 0;
    } else if (napi_is_typedarray(env, value, &is) == napi_ok && is) {
        napi_typedarray_type type;
        if (napi_get_typedarray_info(env, value, &type, &length, &data, NULL, NULL) != napi_ok) return 0;
        static const size_t sizes[] = {
            [napi_int8_array] = 1,      [napi_uint8_array] = 1,     [napi_uint8_clamped_array] = 1,
            [napi_int16_array] = 2,     [napi_uint16_array] = 2,    [napi_int32_array] = 4,
            [napi_uint32_array] = 4,    [napi_float32_array] = 4,   [napi_float64_array] = 8,
            [napi_bigint64_array] = 8,  [napi_biguint64_array] = 8,
        };
        length *= (size_t)type < sizeof sizes / sizeof sizes[0] && sizes[type] ? sizes[type] : 1;
    } else if (napi_is_dataview(env, value, &is) == napi_ok && is) {
        if (napi_get_dataview_info(env, value, &length, &data, NULL, NULL) != napi_ok) return 0;
    } else {
        return 0;
    }
    out->pointer = data;
    out->length = data ? length : 0; /* detached: empty */
    return 1;
}

/* `value` with its toJSON applied, as JSON.stringify applies it (again on
 * what it returns), each call a level of nesting; NULL when it threw or
 * nests past TA_CBOR_DEPTH_MAX (a cycle). */
static napi_value ta_napi_resolve(napi_env env, napi_value value, uint32_t *depth) {
    for (;;) {
        napi_valuetype type = napi_undefined, to_json_type = napi_undefined;
        ta_bytes bytes;
        napi_value to_json, result;
        if (*depth > TA_CBOR_DEPTH_MAX || napi_typeof(env, value, &type) != napi_ok) return NULL;
        if (type != napi_object || ta_napi_values_bytes(env, value, &bytes)) return value;
        if (napi_get_named_property(env, value, "toJSON", &to_json) != napi_ok ||
            napi_typeof(env, to_json, &to_json_type) != napi_ok)
            return NULL;
        if (to_json_type != napi_function) return value;
        if (napi_call_function(env, value, to_json, 0, NULL, &result) != napi_ok) return NULL;
        value = result;
        (*depth)++;
    }
}

/* What JSON.stringify leaves out: undefined, a function, a symbol. */
static int ta_napi_dropped(napi_env env, napi_value value) {
    napi_valuetype type = napi_undefined;
    napi_typeof(env, value, &type);
    return type == napi_undefined || type == napi_function || type == napi_symbol;
}

/* An integer as CBOR has it: `n`, or -1 - n when `negative`. */
typedef struct ta_napi_integer {
    int negative;
    uint64_t n;
} ta_napi_integer;

/* A BigInt within -2^64 … 2^64-1; past it, TA_ERROR_INVALID. */
static ta_status ta_napi_bigint_read(napi_env env, napi_value value, ta_napi_integer *out) {
    int sign = 0;
    uint64_t words[3] = {0, 0, 0};
    size_t count = 3; /* in: room in `words`; out: the words it has */
    if (napi_get_value_bigint_words(env, value, &sign, &count, words) != napi_ok || count > 3) return TA_ERROR_INVALID;
    while (count > 1 && !words[count - 1])
        count--;
    out->negative = sign != 0;
    if (count <= 1 && !sign) out->n = words[0];
    else if (count <= 1 && sign && words[0]) out->n = words[0] - 1;
    else if (count == 2 && sign && !words[0] && words[1] == 1) out->n = UINT64_MAX; /* -2^64 */
    else return TA_ERROR_INVALID;
    return TA_OK;
}

/* A container's own depth may not reach TA_CBOR_DEPTH_MAX: the writer
 * refuses a 65th open container and the reader reads none. */
#define TA_NAPI_OPEN_MAX TA_CBOR_DEPTH_MAX

/* ---- written: the encoder ----------------------------------------------------------- */

void ta_napi_encoder_init(ta_napi_encoder *encoder, napi_env env, const ta_napi_room *kept) {
    ta_napi_room_init(&encoder->text, env, (ta_buffer){encoder->text_stack, sizeof encoder->text_stack, 0});
    ta_buffer room = {encoder->stack, sizeof encoder->stack, 0};
    if (kept && kept->capacity > room.capacity) room = (ta_buffer){kept->pointer, kept->capacity, 0};
    ta_cbor_writer_init(&encoder->writer, room, &encoder->text.allocator);
    encoder->status = TA_OK;
}

void ta_napi_encoder_free(ta_napi_encoder *encoder) {
    ta_cbor_writer_free(&encoder->writer);
    ta_napi_room_free(&encoder->text);
}

static void ta_napi_fail(ta_napi_encoder *encoder, ta_status status) {
    if (encoder->status == TA_OK) encoder->status = status;
}

static int ta_napi_failed(const ta_napi_encoder *encoder) {
    return encoder->status != TA_OK || encoder->writer.status != TA_OK;
}

/* A napi call that failed: an exception is pending (a getter threw), or
 * the value is not one Node-API reads. Either way the value is refused. */
static int ta_napi_ok(ta_napi_encoder *encoder, napi_status status) {
    if (status == napi_ok) return 1;
    ta_napi_fail(encoder, TA_ERROR_INVALID);
    return 0;
}

/* A string as text: through the text room, then into the writer. */
static void ta_napi_write_text(ta_napi_encoder *encoder, napi_value value) {
    size_t length = 0;
    encoder->text.length = 0;
    ta_status status = ta_napi_text_append(&encoder->text, value, &length);
    if (status == TA_OK && length > UINT32_MAX) status = TA_ERROR_INVALID;
    if (status != TA_OK) {
        ta_napi_fail(encoder, status);
        return;
    }
    ta_cbor_write_text(&encoder->writer, (ta_string_view){encoder->text.pointer, (uint32_t)length});
}

/* A number: an integer within ±(2^53-1) is an integer, any other number
 * (-0, NaN, ±Infinity, a fraction, a larger integer) a float. Never
 * napi_get_value_int64, which saturates silently. */
static void ta_napi_write_number(ta_napi_encoder *encoder, double number) {
    uint64_t bits;
    memcpy(&bits, &number, sizeof bits);
    if (number >= -TA_NAPI_SAFE_INTEGER && number <= TA_NAPI_SAFE_INTEGER && number == (double)(int64_t)number &&
        bits != (uint64_t)1 << 63)
        ta_cbor_write_int(&encoder->writer, (int64_t)number);
    else
        ta_cbor_write_float(&encoder->writer, number);
}

static int ta_napi_write(ta_napi_encoder *encoder, napi_value given, uint32_t depth);
static void ta_napi_write_kept(ta_napi_encoder *encoder, napi_value value, uint32_t depth);

static void ta_napi_write_array(ta_napi_encoder *encoder, napi_value value, uint32_t depth) {
    napi_env env = encoder->text.env;
    uint32_t length = 0;
    if (!ta_napi_ok(encoder, napi_get_array_length(env, value, &length))) return;
    ta_cbor_write_array(&encoder->writer, length);
    for (uint32_t i = 0; i < length && !ta_napi_failed(encoder); i++) {
        napi_value item;
        if (!ta_napi_ok(encoder, napi_get_element(env, value, i, &item))) return;
        if (!ta_napi_write(encoder, item, depth + 1)) /* dropped: null in an array */
            ta_cbor_write_simple(&encoder->writer, TA_CBOR_NULL);
    }
}

/* An object's own enumerable string keys, in the order Object.keys gives;
 * a key only for a value that is kept: the map's count is fixed when it
 * closes (TA_CBOR_COUNT_LATER). */
static void ta_napi_write_members(ta_napi_encoder *encoder, napi_value value, uint32_t depth) {
    napi_env env = encoder->text.env;
    napi_value keys;
    uint32_t count = 0;
    if (!ta_napi_ok(encoder, napi_get_all_property_names(env, value, napi_key_own_only,
                                                         napi_key_enumerable | napi_key_skip_symbols,
                                                         napi_key_numbers_to_strings, &keys)) ||
        !ta_napi_ok(encoder, napi_get_array_length(env, keys, &count)))
        return;
    ta_cbor_write_map(&encoder->writer, TA_CBOR_COUNT_LATER);
    for (uint32_t i = 0; i < count && !ta_napi_failed(encoder); i++) {
        napi_value key, item, resolved;
        uint32_t item_depth = depth + 1;
        if (!ta_napi_ok(encoder, napi_get_element(env, keys, i, &key)) ||
            !ta_napi_ok(encoder, napi_get_property(env, value, key, &item)))
            return;
        if (!(resolved = ta_napi_resolve(env, item, &item_depth))) {
            ta_napi_fail(encoder, TA_ERROR_INVALID);
            return;
        }
        if (ta_napi_dropped(env, resolved)) continue; /* dropped: so is its key */
        ta_napi_write_text(encoder, key);
        ta_napi_write_kept(encoder, resolved, item_depth);
    }
    ta_cbor_write_end(&encoder->writer);
}

/* Write `value`, whose toJSON was applied and which is kept. */
static void ta_napi_write_kept(ta_napi_encoder *encoder, napi_value value, uint32_t depth) {
    napi_env env = encoder->text.env;
    napi_valuetype type = napi_undefined;
    if (!ta_napi_ok(encoder, napi_typeof(env, value, &type))) return;
    switch (type) {
    case napi_null: ta_cbor_write_simple(&encoder->writer, TA_CBOR_NULL); break;
    case napi_boolean: {
        bool b = false;
        if (ta_napi_ok(encoder, napi_get_value_bool(env, value, &b)))
            ta_cbor_write_simple(&encoder->writer, b ? TA_CBOR_TRUE : TA_CBOR_FALSE);
        break;
    }
    case napi_number: {
        double number = 0;
        if (ta_napi_ok(encoder, napi_get_value_double(env, value, &number))) ta_napi_write_number(encoder, number);
        break;
    }
    case napi_bigint: {
        ta_napi_integer integer;
        ta_status status = ta_napi_bigint_read(env, value, &integer);
        if (status != TA_OK) ta_napi_fail(encoder, status);
        else if (integer.negative) ta_cbor_write_negative(&encoder->writer, integer.n);
        else ta_cbor_write_uint(&encoder->writer, integer.n);
        break;
    }
    case napi_string: ta_napi_write_text(encoder, value); break;
    case napi_object:
    case napi_external: {
        ta_bytes bytes;
        bool array = false;
        if (ta_napi_values_bytes(env, value, &bytes)) ta_cbor_write_bytes(&encoder->writer, bytes);
        else if (!ta_napi_ok(encoder, napi_is_array(env, value, &array))) return;
        else if (array) ta_napi_write_array(encoder, value, depth);
        else ta_napi_write_members(encoder, value, depth);
        break;
    }
    default: ta_napi_fail(encoder, TA_ERROR_INVALID); break;
    }
}

/* Write `given`: 1 when written, 0 when it is dropped (undefined, a
 * function, a symbol), -1 when it is refused (the status). */
static int ta_napi_write(ta_napi_encoder *encoder, napi_value given, uint32_t depth) {
    if (ta_napi_failed(encoder)) return -1;
    napi_value value = ta_napi_resolve(encoder->text.env, given, &depth);
    if (!value) {
        ta_napi_fail(encoder, TA_ERROR_INVALID);
        return -1;
    }
    if (ta_napi_dropped(encoder->text.env, value)) return 0;
    ta_napi_write_kept(encoder, value, depth);
    return ta_napi_failed(encoder) ? -1 : 1;
}

void ta_napi_encoder_value(ta_napi_encoder *encoder, napi_value value) {
    if (!ta_napi_write(encoder, value, 0)) ta_cbor_write_simple(&encoder->writer, TA_CBOR_NULL);
}

void ta_napi_encoder_member(ta_napi_encoder *encoder, const char *key, napi_value value) {
    uint32_t depth = 1;
    napi_value resolved = ta_napi_resolve(encoder->text.env, value, &depth);
    if (!resolved) {
        ta_napi_fail(encoder, TA_ERROR_INVALID);
        return;
    }
    if (ta_napi_dropped(encoder->text.env, resolved)) return;
    ta_cbor_write_text(&encoder->writer, (ta_string_view){key, (uint32_t)strlen(key)});
    ta_napi_write_kept(encoder, resolved, depth);
}

ta_status ta_napi_encoder_bytes(const ta_napi_encoder *encoder, ta_bytes *out) {
    out->pointer = NULL;
    out->length = 0;
    if (encoder->status != TA_OK) return encoder->status;
    return ta_cbor_writer_bytes(&encoder->writer, out);
}

ta_status ta_napi_encoder_keep(const ta_napi_encoder *encoder, ta_napi_room *kept, ta_bytes *out) {
    ta_status status = ta_napi_encoder_bytes(encoder, out);
    if (status != TA_OK || out->pointer == kept->pointer) return status; /* written in place */
    kept->length = 0;
    if (!ta_napi_room_reserve(kept, out->length)) return TA_ERROR_NO_MEMORY;
    if (out->length) memcpy(kept->pointer, out->pointer, out->length);
    out->pointer = kept->pointer;
    return TA_OK;
}

/* ---- read: a message's values, item by item ------------------------------------------- */

typedef struct ta_napi_decoding {
    napi_env env;
    ta_cbor_reader reader;
} ta_napi_decoding;

static ta_status ta_napi_read(ta_napi_decoding *decoding, napi_value *out);

/* The next item, which must be there. */
static ta_status ta_napi_read_next(ta_napi_decoding *decoding, ta_cbor_item *item) {
    int32_t read = ta_cbor_read(&decoding->reader, item);
    return read == 1 ? TA_OK : read < 0 ? (ta_status)read : TA_ERROR_INVALID;
}

/* -1 - n for n past what a number holds: a BigInt (n + 1 may be 2^64). */
static napi_status ta_napi_negative(napi_env env, uint64_t n, napi_value *out) {
    uint64_t words[2] = {n + 1, n == UINT64_MAX ? 1 : 0};
    return napi_create_bigint_words(env, 1, n == UINT64_MAX ? 2 : 1, words, out);
}

static napi_status ta_napi_text_value(napi_env env, ta_bytes text, napi_value *out) {
    return napi_create_string_utf8(env, text.pointer ? (const char *)text.pointer : "", text.length, out);
}

/* A container's items, read into a new array or object. */
static ta_status ta_napi_read_container(ta_napi_decoding *decoding, const ta_cbor_item *item, napi_value *out) {
    napi_env env = decoding->env;
    if (item->kind == TA_CBOR_ARRAY) {
        if (napi_create_array_with_length(env, item->count, out) != napi_ok) return TA_ERROR_NO_MEMORY;
        for (uint32_t i = 0; i < item->count; i++) {
            napi_value value;
            ta_status status = ta_napi_read(decoding, &value);
            if (status != TA_OK) return status;
            if (napi_set_element(env, *out, i, value) != napi_ok) return TA_ERROR_NO_MEMORY;
        }
        return TA_OK;
    }
    if (napi_create_object(env, out) != napi_ok) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < item->count; i++) {
        ta_cbor_item key;
        napi_property_descriptor property;
        memset(&property, 0, sizeof property);
        ta_status status = ta_napi_read_next(decoding, &key);
        if (status == TA_OK && key.kind != TA_CBOR_TEXT) status = TA_ERROR_INVALID; /* keys are text */
        if (status == TA_OK && ta_napi_text_value(env, key.bytes, &property.name) != napi_ok)
            status = TA_ERROR_NO_MEMORY;
        if (status == TA_OK) status = ta_napi_read(decoding, &property.value);
        if (status != TA_OK) return status;
        /* defined, not set: a key "__proto__" is an ordinary key */
        property.attributes = napi_default_jsproperty;
        if (napi_define_properties(env, *out, 1, &property) != napi_ok) return TA_ERROR_NO_MEMORY;
    }
    return TA_OK;
}

/* The next item's value; TA_ERROR_INVALID when it is not a neutral value. */
static ta_status ta_napi_read(ta_napi_decoding *decoding, napi_value *out) {
    napi_env env = decoding->env;
    ta_cbor_item item;
    ta_status read = ta_napi_read_next(decoding, &item);
    if (read != TA_OK) return read;
    napi_status status = napi_ok;
    switch (item.kind) {
    case TA_CBOR_UINT:
        status = item.value <= TA_NAPI_SAFE_UINT ? napi_create_double(env, (double)item.value, out)
                                                 : napi_create_bigint_uint64(env, item.value, out);
        break;
    case TA_CBOR_NEGINT:
        status = item.value < TA_NAPI_SAFE_UINT ? napi_create_double(env, -1.0 - (double)item.value, out)
                                                : ta_napi_negative(env, item.value, out);
        break;
    case TA_CBOR_BYTES: status = ta_napi_values_uint8array(env, item.bytes, out); break;
    case TA_CBOR_TEXT: status = ta_napi_text_value(env, item.bytes, out); break; /* UTF-8, checked by the reader */
    case TA_CBOR_ARRAY:
    case TA_CBOR_MAP: return ta_napi_read_container(decoding, &item, out);
    case TA_CBOR_TAG: { /* an actor reference: its address */
        ta_cbor_item address;
        if (item.value != TA_CBOR_TAG_URI) return TA_ERROR_INVALID;
        read = ta_napi_read_next(decoding, &address);
        if (read != TA_OK) return read;
        if (address.kind != TA_CBOR_TEXT) return TA_ERROR_INVALID;
        status = ta_napi_text_value(env, address.bytes, out);
        break;
    }
    case TA_CBOR_SIMPLE:
        switch (item.value) {
        case TA_CBOR_FALSE: status = napi_get_boolean(env, false, out); break;
        case TA_CBOR_TRUE: status = napi_get_boolean(env, true, out); break;
        case TA_CBOR_NULL: status = napi_get_null(env, out); break;
        case TA_CBOR_UNDEFINED: status = napi_get_undefined(env, out); break;
        default: return TA_ERROR_INVALID;
        }
        break;
    case TA_CBOR_FLOAT: {
        double number;
        memcpy(&number, &item.value, sizeof number);
        status = napi_create_double(env, number, out);
        break;
    }
    default: return TA_ERROR_INVALID;
    }
    return status == napi_ok ? TA_OK : TA_ERROR_NO_MEMORY;
}

ta_status ta_napi_values_decode(napi_env env, ta_bytes cbor, napi_value *out) {
    ta_napi_decoding decoding;
    decoding.env = env;
    ta_cbor_reader_init(&decoding.reader, cbor);
    ta_status status = ta_napi_read(&decoding, out);
    if (status == TA_OK) { /* the item, whole, and nothing after it */
        ta_cbor_item rest;
        int32_t read = ta_cbor_read(&decoding.reader, &rest);
        if (read) status = read < 0 ? (ta_status)read : TA_ERROR_INVALID;
    }
    return status;
}

/* ---- copied: the value table without CBOR ------------------------------------------------ */

/* A copy in progress: a room for the strings' check, the containers open,
 * the first failure. */
typedef struct ta_napi_copying {
    ta_napi_room room;
    uint32_t open;
    ta_status status;
} ta_napi_copying;

/* A value to copy, and the levels around it. */
typedef struct ta_napi_copied {
    napi_value value;
    uint32_t depth;
} ta_napi_copied;

static int ta_napi_copy(ta_napi_copying *copying, const ta_napi_copied *given, napi_value *out);

static napi_value ta_napi_copy_failed(ta_napi_copying *copying, ta_status status) {
    if (copying->status == TA_OK) copying->status = status;
    return NULL;
}

/* A string is shared (strings do not change), once checked as encode
 * checks it: no lone surrogate. */
static napi_value ta_napi_copy_text(ta_napi_copying *copying, napi_value value) {
    size_t length = 0;
    copying->room.length = 0;
    ta_status status = ta_napi_text_append(&copying->room, value, &length);
    return status == TA_OK ? value : ta_napi_copy_failed(copying, status);
}

/* A BigInt as decode gives it back: a number within ±(2^53-1), else the
 * BigInt itself. */
static napi_value ta_napi_copy_bigint(ta_napi_copying *copying, napi_value value) {
    napi_env env = copying->room.env;
    ta_napi_integer integer;
    napi_value number = NULL;
    ta_status status = ta_napi_bigint_read(env, value, &integer);
    if (status != TA_OK) return ta_napi_copy_failed(copying, status);
    if (!integer.negative && integer.n <= TA_NAPI_SAFE_UINT) napi_create_double(env, (double)integer.n, &number);
    else if (integer.negative && integer.n < TA_NAPI_SAFE_UINT) napi_create_double(env, -1.0 - (double)integer.n, &number);
    else return value;
    return number ? number : ta_napi_copy_failed(copying, TA_ERROR_NO_MEMORY);
}

static napi_value ta_napi_copy_array(ta_napi_copying *copying, const ta_napi_copied *array) {
    napi_env env = copying->room.env;
    uint32_t length = 0;
    napi_value copy, null;
    napi_get_null(env, &null);
    if (napi_get_array_length(env, array->value, &length) != napi_ok ||
        napi_create_array_with_length(env, length, &copy) != napi_ok)
        return ta_napi_copy_failed(copying, TA_ERROR_INVALID);
    for (uint32_t i = 0; i < length; i++) {
        ta_napi_copied item = {NULL, array->depth + 1};
        napi_value value = null;
        if (napi_get_element(env, array->value, i, &item.value) != napi_ok ||
            ta_napi_copy(copying, &item, &value) < 0 || napi_set_element(env, copy, i, value) != napi_ok)
            return ta_napi_copy_failed(copying, TA_ERROR_INVALID); /* dropped: null in an array */
    }
    return copy;
}

static napi_value ta_napi_copy_members(ta_napi_copying *copying, const ta_napi_copied *object) {
    napi_env env = copying->room.env;
    napi_value keys, copy;
    uint32_t count = 0;
    if (napi_get_all_property_names(env, object->value, napi_key_own_only, napi_key_enumerable | napi_key_skip_symbols,
                                    napi_key_numbers_to_strings, &keys) != napi_ok ||
        napi_get_array_length(env, keys, &count) != napi_ok || napi_create_object(env, &copy) != napi_ok)
        return ta_napi_copy_failed(copying, TA_ERROR_INVALID);
    for (uint32_t i = 0; i < count; i++) {
        napi_property_descriptor property;
        memset(&property, 0, sizeof property);
        ta_napi_copied item = {NULL, object->depth + 1};
        if (napi_get_element(env, keys, i, &property.name) != napi_ok ||
            napi_get_property(env, object->value, property.name, &item.value) != napi_ok)
            return ta_napi_copy_failed(copying, TA_ERROR_INVALID);
        int copied = ta_napi_copy(copying, &item, &property.value);
        if (copied < 0) return NULL;
        if (!copied) continue; /* dropped: so is its key */
        if (!ta_napi_copy_text(copying, property.name)) return NULL;
        /* defined, not set, as decode does: "__proto__" is an ordinary key */
        property.attributes = napi_default_jsproperty;
        if (napi_define_properties(env, copy, 1, &property) != napi_ok)
            return ta_napi_copy_failed(copying, TA_ERROR_NO_MEMORY);
    }
    return copy;
}

/* A copy of a value whose toJSON was applied and which is kept. */
static napi_value ta_napi_copy_kept(ta_napi_copying *copying, const ta_napi_copied *kept) {
    napi_env env = copying->room.env;
    napi_valuetype type = napi_undefined;
    ta_bytes bytes;
    bool array = false;
    if (napi_typeof(env, kept->value, &type) != napi_ok) return ta_napi_copy_failed(copying, TA_ERROR_INVALID);
    switch (type) {
    case napi_null:
    case napi_boolean:
    case napi_number: return kept->value; /* a number is what decode gives back */
    case napi_bigint: return ta_napi_copy_bigint(copying, kept->value);
    case napi_string: return ta_napi_copy_text(copying, kept->value);
    case napi_object:
    case napi_external: break;
    default: return ta_napi_copy_failed(copying, TA_ERROR_INVALID);
    }
    if (ta_napi_values_bytes(env, kept->value, &bytes)) {
        napi_value copy;
        if (ta_napi_values_uint8array(env, bytes, &copy) != napi_ok) return ta_napi_copy_failed(copying, TA_ERROR_NO_MEMORY);
        return copy;
    }
    if (napi_is_array(env, kept->value, &array) != napi_ok) return ta_napi_copy_failed(copying, TA_ERROR_INVALID);
    if (copying->open == TA_NAPI_OPEN_MAX) return ta_napi_copy_failed(copying, TA_ERROR_INVALID);
    copying->open++;
    napi_value copy = array ? ta_napi_copy_array(copying, kept) : ta_napi_copy_members(copying, kept);
    copying->open--;
    return copy;
}

/* 1 with the copy in *out, 0 when it is dropped, -1 when it is refused. */
static int ta_napi_copy(ta_napi_copying *copying, const ta_napi_copied *given, napi_value *out) {
    ta_napi_copied kept = *given;
    if (!(kept.value = ta_napi_resolve(copying->room.env, given->value, &kept.depth))) {
        ta_napi_copy_failed(copying, TA_ERROR_INVALID);
        return -1;
    }
    if (ta_napi_dropped(copying->room.env, kept.value)) return 0;
    napi_value copy = ta_napi_copy_kept(copying, &kept);
    if (!copy) return -1;
    *out = copy;
    return 1;
}

ta_status ta_napi_values_copy(napi_env env, napi_value value, napi_value *out) {
    char stack[256];
    ta_napi_copying copying;
    ta_napi_room_init(&copying.room, env, (ta_buffer){stack, sizeof stack, 0});
    copying.open = 0;
    copying.status = TA_OK;
    ta_napi_copied given = {value, 0};
    int copied = ta_napi_copy(&copying, &given, out);
    ta_napi_room_free(&copying.room);
    if (copied < 0) return copying.status != TA_OK ? copying.status : TA_ERROR_INVALID;
    if (!copied) napi_get_null(env, out); /* dropped at the top: null */
    return TA_OK;
}

/* ---- the exports ------------------------------------------------------------- */

/* encode(value) -> Uint8Array: the value written, then copied out. */
napi_value ta_napi_encode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], result = NULL;
    napi_get_undefined(env, &argv[0]);
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
    ta_napi_encoder encoder;
    ta_napi_encoder_init(&encoder, env, NULL);
    ta_napi_encoder_value(&encoder, argv[0]);
    ta_bytes cbor;
    ta_status status = ta_napi_encoder_bytes(&encoder, &cbor);
    if (status != TA_OK) ta_napi_throw_status(env, "encode", status);
    else if (ta_napi_values_uint8array(env, cbor, &result) != napi_ok) result = NULL;
    ta_napi_encoder_free(&encoder);
    return result;
}

/* decode(bytes) -> value: the bytes, borrowed, read item by item. */
napi_value ta_napi_decode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], result = NULL;
    napi_get_undefined(env, &argv[0]);
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
    ta_bytes cbor;
    if (!ta_napi_values_bytes(env, argv[0], &cbor)) {
        ta_napi_throw_status(env, "decode", TA_ERROR_INVALID);
        return NULL;
    }
    ta_status status = ta_napi_values_decode(env, cbor, &result);
    if (status != TA_OK) {
        ta_napi_throw_status(env, "decode", status);
        return NULL;
    }
    return result;
}

/* ---- for the API ----------------------------------------------------------------- */

napi_status ta_napi_values_uint8array(napi_env env, ta_bytes bytes, napi_value *out) {
    void *data = NULL;
    napi_value buffer;
    napi_status status = napi_create_arraybuffer(env, bytes.length, &data, &buffer);
    if (status == napi_ok && bytes.length) memcpy(data, bytes.pointer, bytes.length);
    return status == napi_ok ? napi_create_typedarray(env, napi_uint8_array, bytes.length, buffer, 0, out) : status;
}

/* The native datamodel: a copy; quickjs: CBOR through its from_wire. */
ta_status ta_napi_values_payload(ta_napi_host *host, napi_value value, ta_payload *out) {
    napi_valuetype type = napi_undefined;
    *out = 0;
    if (napi_typeof(host->env, value, &type) != napi_ok || type == napi_undefined) return TA_OK;
    if (!host->quickjs) return ta_napi_datamodel_payload(&host->native, value, out);
    ta_napi_encoder encoder;
    ta_napi_encoder_init(&encoder, host->env, NULL);
    ta_napi_encoder_value(&encoder, value);
    ta_bytes cbor;
    ta_status status = ta_napi_encoder_bytes(&encoder, &cbor);
    if (status == TA_OK) status = host->binding.vtable->from_wire(host->binding.datamodel, cbor, out);
    ta_napi_encoder_free(&encoder);
    return status;
}

/* The native datamodel: a copy; quickjs: its to_wire's view, read. */
ta_status ta_napi_values_of_payload(ta_napi_host *host, const ta_napi_payload_read *read, napi_value *out) {
    if (!read->payload || read->datamodel != 0)
        return napi_get_undefined(host->env, out) == napi_ok ? TA_OK : TA_ERROR_NO_MEMORY;
    if (!host->quickjs) return ta_napi_datamodel_value(&host->native, read->payload, out);
    ta_bytes cbor;
    ta_status status = host->binding.vtable->to_wire(host->binding.datamodel, read->payload, &cbor);
    return status == TA_OK ? ta_napi_values_decode(host->env, cbor, out) : status;
}
