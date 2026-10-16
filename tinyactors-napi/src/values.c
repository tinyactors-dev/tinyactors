/* The value table (docs/values.md; tinyactors-core/docs/protocol.md,
 * "Values"): JavaScript values to CBOR and back, streaming with core's
 * ta_cbor_writer and ta_cbor_reader. The one conversion the API uses:
 * encode and decode here, and later the config, typed methods' params and
 * replies, and payloads for quickjs.
 *
 * Core owns CBOR's syntax and checks (UTF-8, widths, nesting); this file
 * owns the JavaScript mapping:
 *
 *   undefined, null, true, false  simple values 23, 22, 21, 20
 *   a number                      an integer when Number.isSafeInteger holds
 *                                 and it is not -0, else a float
 *   a bigint                      an integer, -2^64 to 2^64-1
 *   a string                      text, valid UTF-8: a lone surrogate refused
 *   a Uint8Array (a Buffer too)   bytes
 *   an array                      an array, holes as undefined
 *   a plain object                a map of its own enumerable string keys,
 *                                 in Object.keys order
 *
 * and back: integers within +-(2^53-1) numbers, others bigints; bytes a
 * Uint8Array; tag 32 on text (an actor reference) its address string, never
 * written. Anything else is refused: TinyactorsError with TA_ERROR_UNSUPPORTED
 * for a value outside the table (a function, a symbol, a Date, a class's
 * instance), TA_ERROR_INVALID for one the table cannot hold (a bigint past
 * 64 bits, a lone surrogate, nesting deeper than 64, which a cycle reaches). */
#include "addon.h"

#include <string.h>

/* 2^53 - 1, Number.MAX_SAFE_INTEGER. */
#define TA_NAPI_SAFE_INTEGER 9007199254740991.0

/* One encode: the writer, the environment's instance, and the first
 * refusal. `thrown` when a JavaScript exception is pending instead (a
 * getter threw, or Node-API failed and set one). */
typedef struct ta_napi_encoder {
    napi_env env;
    ta_napi_instance *instance;
    ta_cbor_writer *writer;
    ta_napi_refusal refusal;
    bool thrown;
} ta_napi_encoder;

static bool ta_napi_refuse(ta_napi_encoder *encoder, ta_status status, const char *message) {
    encoder->refusal = (ta_napi_refusal){status, message};
    return false;
}

/* A Node-API call failed: its exception, or one saying so. */
static bool ta_napi_failed(ta_napi_encoder *encoder) {
    bool pending = false;
    napi_is_exception_pending(encoder->env, &pending);
    if (pending) {
        encoder->thrown = true;
        return false;
    }
    return ta_napi_refuse(encoder, TA_ERROR_FATAL, "a Node-API call failed while encoding a value");
}

#define TA_NAPI_CHECK(encoder, call) \
    do { if ((call) != napi_ok) return ta_napi_failed(encoder); } while (0)

/* Whether the UTF-16 code units `utf16` hold a surrogate that is not half
 * of a pair. */
static bool ta_napi_lone_surrogate(ta_bytes utf16) {
    const char16_t *text = (const char16_t *)utf16.pointer;
    size_t length = utf16.length / sizeof *text;
    for (size_t i = 0; i < length; i++) {
        char16_t unit = text[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 < length && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) i++;
            else return true;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) return true;
    }
    return false;
}

/* The string `value` as UTF-8 in the call's arena (the copy Node-API
 * forces), into *out. Node-API writes a lone surrogate as U+FFFD (EF BF
 * BD), so when those bytes appear the string is read again as UTF-16 and
 * checked: a lone surrogate is refused, never replaced. */
static bool ta_napi_text(ta_napi_encoder *encoder, napi_value value, ta_string_view *out) {
    napi_env env = encoder->env;
    size_t length = 0;
    TA_NAPI_CHECK(encoder, napi_get_value_string_utf8(env, value, NULL, 0, &length));
    if (length > UINT32_MAX) return ta_napi_refuse(encoder, TA_ERROR_INVALID, "a string longer than 2^32-1 bytes");
    char *text = (char *)ta_growable_arena_alloc(&encoder->instance->arena, length + 1, 1);
    if (text == NULL) return ta_napi_refuse(encoder, TA_ERROR_NO_MEMORY, "no memory for a string");
    size_t written = 0;
    TA_NAPI_CHECK(encoder, napi_get_value_string_utf8(env, value, text, length + 1, &written));
    for (const char *at = text; (at = (const char *)memchr(at, 0xEF, (size_t)(text + written - at))) != NULL; at++) {
        if (text + written - at < 3 || (unsigned char)at[1] != 0xBF || (unsigned char)at[2] != 0xBD) continue;
        size_t units = 0;
        TA_NAPI_CHECK(encoder, napi_get_value_string_utf16(env, value, NULL, 0, &units));
        char16_t *wide = (char16_t *)ta_growable_arena_alloc(&encoder->instance->arena, (units + 1) * sizeof *wide, 2);
        if (wide == NULL) return ta_napi_refuse(encoder, TA_ERROR_NO_MEMORY, "no memory for a string");
        size_t read = 0;
        TA_NAPI_CHECK(encoder, napi_get_value_string_utf16(env, value, wide, units + 1, &read));
        if (ta_napi_lone_surrogate((ta_bytes){wide, read * sizeof *wide}))
            return ta_napi_refuse(encoder, TA_ERROR_INVALID, "a string with a lone surrogate is not text");
        break;
    }
    *out = (ta_string_view){text, (uint32_t)written};
    return true;
}

/* A number: an integer when Number.isSafeInteger(value) and not -0, else a
 * float (NaN, the infinities, -0, fractions, and integers past 2^53). */
static bool ta_napi_write_number(ta_napi_encoder *encoder, napi_value value) {
    double number;
    TA_NAPI_CHECK(encoder, napi_get_value_double(encoder->env, value, &number));
    bool safe = number >= -TA_NAPI_SAFE_INTEGER && number <= TA_NAPI_SAFE_INTEGER &&
                (double)(int64_t)number == number && !(number == 0 && 1 / number < 0);
    if (safe) ta_cbor_write_int(encoder->writer, (int64_t)number);
    else ta_cbor_write_float(encoder->writer, number);
    return true;
}

/* A bigint: an integer from -2^64 to 2^64-1, else TA_ERROR_INVALID. */
static bool ta_napi_write_bigint(ta_napi_encoder *encoder, napi_value value) {
    size_t count = 0;
    TA_NAPI_CHECK(encoder, napi_get_value_bigint_words(encoder->env, value, NULL, &count, NULL));
    if (count > 2) return ta_napi_refuse(encoder, TA_ERROR_INVALID, "a bigint past 64 bits");
    int sign = 0;
    uint64_t words[2] = {0, 0};
    TA_NAPI_CHECK(encoder, napi_get_value_bigint_words(encoder->env, value, &sign, &count, words));
    if (!sign) {
        if (count == 2 && words[1] != 0) return ta_napi_refuse(encoder, TA_ERROR_INVALID, "a bigint past 64 bits");
        ta_cbor_write_uint(encoder->writer, words[0]);
    } else if (count == 2 && words[1] != 0) {
        if (words[1] != 1 || words[0] != 0) return ta_napi_refuse(encoder, TA_ERROR_INVALID, "a bigint past 64 bits");
        ta_cbor_write_negative(encoder->writer, UINT64_MAX); /* -2^64 */
    } else if (words[0] == 0) ta_cbor_write_uint(encoder->writer, 0); /* -0n is 0n */
    else ta_cbor_write_negative(encoder->writer, words[0] - 1);
    return true;
}

static bool ta_napi_write(ta_napi_encoder *encoder, napi_value value, uint32_t depth);

static bool ta_napi_write_array(ta_napi_encoder *encoder, napi_value array, uint32_t depth) {
    napi_env env = encoder->env;
    uint32_t length = 0;
    TA_NAPI_CHECK(encoder, napi_get_array_length(env, array, &length));
    ta_cbor_write_array(encoder->writer, TA_CBOR_COUNT_LATER);
    for (uint32_t i = 0; i < length; i++) {
        napi_value item;
        TA_NAPI_CHECK(encoder, napi_get_element(env, array, i, &item));
        if (!ta_napi_write(encoder, item, depth + 1)) return false;
    }
    ta_cbor_write_end(encoder->writer);
    return true;
}

/* A plain object (its prototype Object.prototype or null): its own
 * enumerable string keys in Object.keys order, each with its value. */
static bool ta_napi_write_object(ta_napi_encoder *encoder, napi_value object, uint32_t depth) {
    napi_env env = encoder->env;
    napi_value prototype, plain, keys;
    napi_valuetype kind;
    bool same = false;
    TA_NAPI_CHECK(encoder, napi_get_prototype(env, object, &prototype));
    TA_NAPI_CHECK(encoder, napi_typeof(env, prototype, &kind));
    if (kind != napi_null) {
        TA_NAPI_CHECK(encoder, napi_get_reference_value(env, encoder->instance->object, &plain));
        TA_NAPI_CHECK(encoder, napi_strict_equals(env, prototype, plain, &same));
        if (!same)
            return ta_napi_refuse(encoder, TA_ERROR_UNSUPPORTED,
                                  "only plain objects, arrays and Uint8Arrays are values (docs/values.md)");
    }
    TA_NAPI_CHECK(encoder, napi_get_all_property_names(env, object, napi_key_own_only,
                                                       napi_key_enumerable | napi_key_skip_symbols,
                                                       napi_key_numbers_to_strings, &keys));
    uint32_t count = 0;
    TA_NAPI_CHECK(encoder, napi_get_array_length(env, keys, &count));
    ta_cbor_write_map(encoder->writer, TA_CBOR_COUNT_LATER);
    for (uint32_t i = 0; i < count; i++) {
        napi_value key, item;
        ta_string_view text;
        TA_NAPI_CHECK(encoder, napi_get_element(env, keys, i, &key));
        if (!ta_napi_text(encoder, key, &text)) return false;
        ta_cbor_write_text(encoder->writer, text);
        TA_NAPI_CHECK(encoder, napi_get_property(env, object, key, &item));
        if (!ta_napi_write(encoder, item, depth + 1)) return false;
    }
    ta_cbor_write_end(encoder->writer);
    return true;
}

/* One value at `depth` (0 for the whole value), appended to the writer.
 * False on a refusal or a pending exception; the writer is then partial. */
static bool ta_napi_write(ta_napi_encoder *encoder, napi_value value, uint32_t depth) {
    napi_env env = encoder->env;
    ta_cbor_writer *writer = encoder->writer;
    if (depth > TA_CBOR_DEPTH_MAX)
        return ta_napi_refuse(encoder, TA_ERROR_INVALID, "a value nested deeper than 64 (or a cycle)");
    napi_valuetype kind;
    TA_NAPI_CHECK(encoder, napi_typeof(env, value, &kind));
    switch (kind) {
    case napi_undefined: ta_cbor_write_simple(writer, TA_CBOR_UNDEFINED); break;
    case napi_null: ta_cbor_write_simple(writer, TA_CBOR_NULL); break;
    case napi_boolean: {
        bool truth;
        TA_NAPI_CHECK(encoder, napi_get_value_bool(env, value, &truth));
        ta_cbor_write_simple(writer, truth ? TA_CBOR_TRUE : TA_CBOR_FALSE);
        break;
    }
    case napi_number:
        if (!ta_napi_write_number(encoder, value)) return false;
        break;
    case napi_bigint:
        if (!ta_napi_write_bigint(encoder, value)) return false;
        break;
    case napi_string: {
        ta_string_view text;
        if (!ta_napi_text(encoder, value, &text)) return false;
        ta_cbor_write_text(writer, text);
        break;
    }
    case napi_object: {
        bool is = false;
        TA_NAPI_CHECK(encoder, napi_is_array(env, value, &is));
        if (is) {
            if (!ta_napi_write_array(encoder, value, depth)) return false;
            break;
        }
        TA_NAPI_CHECK(encoder, napi_is_typedarray(env, value, &is));
        if (is) {
            napi_typedarray_type type;
            size_t length;
            void *data;
            TA_NAPI_CHECK(encoder, napi_get_typedarray_info(env, value, &type, &length, &data, NULL, NULL));
            if (type != napi_uint8_array)
                return ta_napi_refuse(encoder, TA_ERROR_UNSUPPORTED, "bytes are a Uint8Array, not another typed array");
            ta_cbor_write_bytes(writer, (ta_bytes){data, length});
            break;
        }
        if (!ta_napi_write_object(encoder, value, depth)) return false;
        break;
    }
    case napi_symbol:
    case napi_function:
    case napi_external:
    default:
        return ta_napi_refuse(encoder, TA_ERROR_UNSUPPORTED,
                              "a function, a symbol or an external is not a value (docs/values.md)");
    }
    if (writer->status != TA_OK)
        return ta_napi_refuse(encoder, writer->status,
                              writer->status == TA_ERROR_NO_MEMORY ? "no memory for the encoded value"
                                                                   : "the CBOR writer refused the value");
    return true;
}

bool ta_napi_value_write(napi_env env, napi_value value, ta_cbor_writer *writer) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (instance == NULL) return false;
    ta_napi_encoder encoder = {env, instance, writer, {TA_OK, NULL}, false};
    if (ta_napi_write(&encoder, value, 0)) return true;
    if (!encoder.thrown) ta_napi_throw(env, &encoder.refusal);
    return false;
}

/* encode(value) -> Uint8Array: the value's CBOR, copied once out of the
 * call's arena. */
napi_value ta_napi_encode(napi_env env, napi_callback_info info) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (instance == NULL) return NULL;
    size_t argc = 1;
    napi_value argument = NULL, result = NULL;
    if (napi_get_cb_info(env, info, &argc, &argument, NULL, NULL) != napi_ok) return NULL;
    if (argc < 1 && napi_get_undefined(env, &argument) != napi_ok) return NULL;
    ta_growable_arena_mark mark = ta_growable_arena_position(&instance->arena);
    ta_allocator scratch = ta_growable_arena_allocator(&instance->arena);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, &scratch);
    if (ta_napi_value_write(env, argument, &writer)) {
        ta_bytes bytes;
        ta_status status = ta_cbor_writer_bytes(&writer, &bytes);
        void *data = NULL;
        napi_value buffer;
        if (status != TA_OK) ta_napi_throw(env, &(ta_napi_refusal){status, "the CBOR writer refused the value"});
        else if (napi_create_arraybuffer(env, bytes.length, &data, &buffer) == napi_ok) {
            if (bytes.length > 0) memcpy(data, bytes.pointer, bytes.length);
            if (napi_create_typedarray(env, napi_uint8_array, bytes.length, buffer, 0, &result) != napi_ok)
                result = NULL;
        }
    }
    ta_cbor_writer_free(&writer);
    ta_growable_arena_release(&instance->arena, mark);
    return result;
}

/* ---- reading -------------------------------------------------------------- */

/* One decode: the reader over borrowed bytes. No JavaScript runs while it
 * reads (values are made, properties defined, never set), so the bytes
 * cannot change under it. */
typedef struct ta_napi_decoder {
    napi_env env;
    ta_cbor_reader reader;
    ta_napi_refusal refusal;
    bool thrown;
} ta_napi_decoder;

static napi_value ta_napi_reject(ta_napi_decoder *decoder, ta_status status, const char *message) {
    decoder->refusal = (ta_napi_refusal){status, message};
    return NULL;
}

static napi_value ta_napi_broke(ta_napi_decoder *decoder) {
    bool pending = false;
    napi_is_exception_pending(decoder->env, &pending);
    if (pending) decoder->thrown = true;
    else ta_napi_reject(decoder, TA_ERROR_FATAL, "a Node-API call failed while decoding a value");
    return NULL;
}

#define TA_NAPI_MAKE(decoder, call) \
    do { if ((call) != napi_ok) return ta_napi_broke(decoder); } while (0)

static napi_value ta_napi_read(ta_napi_decoder *decoder);

/* -1 - n: a number within -(2^53-1), else a bigint (down to -2^64). */
static napi_value ta_napi_read_negative(ta_napi_decoder *decoder, uint64_t n) {
    napi_value out;
    if (n < (uint64_t)TA_NAPI_SAFE_INTEGER) TA_NAPI_MAKE(decoder, napi_create_double(decoder->env, -1.0 - (double)n, &out));
    else if (n < UINT64_MAX) TA_NAPI_MAKE(decoder, napi_create_bigint_words(decoder->env, 1, 1, (uint64_t[]){n + 1}, &out));
    else TA_NAPI_MAKE(decoder, napi_create_bigint_words(decoder->env, 1, 2, (uint64_t[]){0, 1}, &out));
    return out;
}

static napi_value ta_napi_read_array(ta_napi_decoder *decoder, uint32_t count) {
    napi_value array;
    TA_NAPI_MAKE(decoder, napi_create_array_with_length(decoder->env, count, &array));
    for (uint32_t i = 0; i < count; i++) {
        napi_value item = ta_napi_read(decoder);
        if (item == NULL) return NULL;
        TA_NAPI_MAKE(decoder, napi_set_element(decoder->env, array, i, item));
    }
    return array;
}

/* A map: a plain object whose keys are defined, not set, so "__proto__"
 * is an own key and no setter runs; a key given twice keeps its last value. */
static napi_value ta_napi_read_map(ta_napi_decoder *decoder, uint32_t count) {
    napi_env env = decoder->env;
    napi_value object;
    TA_NAPI_MAKE(decoder, napi_create_object(env, &object));
    for (uint32_t i = 0; i < count; i++) {
        ta_cbor_item key;
        int32_t read = ta_cbor_read(&decoder->reader, &key);
        if (read != 1) return ta_napi_reject(decoder, read < 0 ? read : TA_ERROR_INVALID, "malformed or truncated CBOR");
        if (key.kind != TA_CBOR_TEXT) return ta_napi_reject(decoder, TA_ERROR_INVALID, "a map key that is not text");
        napi_property_descriptor property = {NULL, NULL, NULL, NULL, NULL, NULL,
                                             napi_writable | napi_enumerable | napi_configurable, NULL};
        TA_NAPI_MAKE(decoder, napi_create_string_utf8(env, (const char *)key.bytes.pointer, key.bytes.length, &property.name));
        property.value = ta_napi_read(decoder);
        if (property.value == NULL) return NULL;
        TA_NAPI_MAKE(decoder, napi_define_properties(env, object, 1, &property));
    }
    return object;
}

/* The next item and its children, as a JavaScript value; NULL with the
 * refusal or a pending exception. */
static napi_value ta_napi_read(ta_napi_decoder *decoder) {
    napi_env env = decoder->env;
    ta_cbor_item item;
    int32_t read = ta_cbor_read(&decoder->reader, &item);
    if (read != 1) return ta_napi_reject(decoder, read < 0 ? read : TA_ERROR_INVALID, "malformed or truncated CBOR");
    napi_value out;
    switch (item.kind) {
    case TA_CBOR_UINT:
        if (item.value <= (uint64_t)TA_NAPI_SAFE_INTEGER) TA_NAPI_MAKE(decoder, napi_create_double(env, (double)item.value, &out));
        else TA_NAPI_MAKE(decoder, napi_create_bigint_uint64(env, item.value, &out));
        return out;
    case TA_CBOR_NEGINT: return ta_napi_read_negative(decoder, item.value);
    case TA_CBOR_FLOAT: {
        double number;
        memcpy(&number, &item.value, sizeof number);
        TA_NAPI_MAKE(decoder, napi_create_double(env, number, &out));
        return out;
    }
    case TA_CBOR_BYTES: {
        void *data = NULL;
        napi_value buffer;
        TA_NAPI_MAKE(decoder, napi_create_arraybuffer(env, item.bytes.length, &data, &buffer));
        if (item.bytes.length > 0) memcpy(data, item.bytes.pointer, item.bytes.length);
        TA_NAPI_MAKE(decoder, napi_create_typedarray(env, napi_uint8_array, item.bytes.length, buffer, 0, &out));
        return out;
    }
    case TA_CBOR_TEXT:
        TA_NAPI_MAKE(decoder, napi_create_string_utf8(env, (const char *)item.bytes.pointer, item.bytes.length, &out));
        return out;
    case TA_CBOR_TAG: {
        /* An actor reference: tag 32 on text, read as its address string. */
        ta_cbor_item child;
        if (item.value != TA_CBOR_TAG_URI) return ta_napi_reject(decoder, TA_ERROR_INVALID, "a tag other than 32");
        read = ta_cbor_read(&decoder->reader, &child);
        if (read != 1) return ta_napi_reject(decoder, read < 0 ? read : TA_ERROR_INVALID, "malformed or truncated CBOR");
        if (child.kind != TA_CBOR_TEXT) return ta_napi_reject(decoder, TA_ERROR_INVALID, "tag 32 on something but text");
        TA_NAPI_MAKE(decoder, napi_create_string_utf8(env, (const char *)child.bytes.pointer, child.bytes.length, &out));
        return out;
    }
    case TA_CBOR_SIMPLE:
        switch (item.value) {
        case TA_CBOR_UNDEFINED: TA_NAPI_MAKE(decoder, napi_get_undefined(env, &out)); return out;
        case TA_CBOR_NULL: TA_NAPI_MAKE(decoder, napi_get_null(env, &out)); return out;
        case TA_CBOR_TRUE:
        case TA_CBOR_FALSE: TA_NAPI_MAKE(decoder, napi_get_boolean(env, item.value == TA_CBOR_TRUE, &out)); return out;
        default: return ta_napi_reject(decoder, TA_ERROR_INVALID, "a simple value other than false, true, null, undefined");
        }
    case TA_CBOR_ARRAY: return ta_napi_read_array(decoder, item.count);
    case TA_CBOR_MAP: return ta_napi_read_map(decoder, item.count);
    default: return ta_napi_reject(decoder, TA_ERROR_INVALID, "an item the value table has no row for");
    }
}

napi_value ta_napi_value_read(napi_env env, ta_bytes cbor) {
    ta_napi_decoder decoder = {.env = env};
    ta_cbor_reader_init(&decoder.reader, cbor);
    napi_value value = ta_napi_read(&decoder);
    if (value != NULL) {
        ta_cbor_item extra;
        int32_t read = ta_cbor_read(&decoder.reader, &extra);
        if (read != 0) value = ta_napi_reject(&decoder, read < 0 ? read : TA_ERROR_INVALID, "bytes after the item");
    }
    if (value == NULL && !decoder.thrown) ta_napi_throw(env, &decoder.refusal);
    return value;
}

/* decode(bytes) -> value: bytes a Uint8Array (a Buffer too) or an
 * ArrayBuffer, read during the call. */
napi_value ta_napi_decode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argument = NULL;
    if (napi_get_cb_info(env, info, &argc, &argument, NULL, NULL) != napi_ok) return NULL;
    bool is = false;
    void *data = NULL;
    size_t length = 0;
    if (argc >= 1 && napi_is_typedarray(env, argument, &is) == napi_ok && is) {
        napi_typedarray_type type;
        if (napi_get_typedarray_info(env, argument, &type, &length, &data, NULL, NULL) != napi_ok) return NULL;
        is = type == napi_uint8_array;
    } else if (argc >= 1 && napi_is_arraybuffer(env, argument, &is) == napi_ok && is) {
        if (napi_get_arraybuffer_info(env, argument, &data, &length) != napi_ok) return NULL;
    }
    if (!is) {
        napi_throw_type_error(env, NULL, "decode takes a Uint8Array or an ArrayBuffer");
        return NULL;
    }
    return ta_napi_value_read(env, (ta_bytes){data, length});
}
