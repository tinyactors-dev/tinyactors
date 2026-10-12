/* values.c — CBOR tokens and JavaScript values, by the value table of
 * docs/values.md: one conversion for the API (config, requests, replies,
 * event data), the native datamodel's to_wire and from_wire, and the
 * package's `encode` and `decode` exports.
 *
 * Read (decode): one ta_cbor_decode into a token list, then the values.
 * Written (encode): the value's tokens, then one ta_cbor_encode. The rules
 * are those of core's ECMAScript datamodel, so both of Node's datamodels
 * read and write the same values
 * (tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c,
 * wire_write and wire_read): what JSON.stringify keeps, with integers,
 * BigInts and bytes exact; an actor reference (tag 32) read as its
 * address text and never written. */
#include "addon.h"

#include <string.h>

/* 2^53 - 1: the integers a number holds exactly. */
#define TA_NAPI_SAFE_INTEGER 9007199254740991.0
#define TA_NAPI_SAFE_UINT ((uint64_t)9007199254740991)

/* ---- written: a value's tokens ------------------------------------------------ */

/* A value being written: its tokens (scratch->tokens) and the text and
 * bytes they point into (scratch->strings), both reused by every encode on
 * the scratch. */
typedef struct ta_napi_writer {
    napi_env env;
    ta_napi_scratch *scratch;
    uint32_t count;
    size_t length;
    ta_status status;
} ta_napi_writer;

/* Append a token; its index, or TA_NIL (and the status set). */
static uint32_t ta_napi_writer_token(ta_napi_writer *writer, const ta_cbor_token *token) {
    if (writer->status != TA_OK) return TA_NIL;
    ta_napi_buffer *tokens = &writer->scratch->tokens;
    if (!ta_napi_buffer_reserve(tokens, ((size_t)writer->count + 1) * sizeof(ta_cbor_token))) {
        writer->status = TA_ERROR_NO_MEMORY;
        return TA_NIL;
    }
    ((ta_cbor_token *)tokens->pointer)[writer->count] = *token;
    return writer->count++;
}

/* Room for `more` bytes of text or bytes; a pointer to it, or NULL. */
static char *ta_napi_writer_room(ta_napi_writer *writer, size_t more) {
    if (writer->status != TA_OK) return NULL;
    if (more > UINT32_MAX) {
        writer->status = TA_ERROR_INVALID;
        return NULL;
    }
    ta_napi_buffer *strings = &writer->scratch->strings;
    if (!ta_napi_buffer_reserve(strings, writer->length + more + 1)) {
        writer->status = TA_ERROR_NO_MEMORY;
        return NULL;
    }
    return strings->pointer + writer->length;
}

/* A text or byte token over the `length` bytes just written at the end of
 * the strings. */
typedef struct ta_napi_written {
    uint32_t kind;
    size_t length;
} ta_napi_written;

static void ta_napi_writer_string(ta_napi_writer *writer, const ta_napi_written *written) {
    ta_cbor_token token = {written->kind, (uint32_t)written->length, writer->length};
    if (ta_napi_writer_token(writer, &token) == TA_NIL) return;
    writer->length += written->length;
}

static void ta_napi_writer_bytes(ta_napi_writer *writer, ta_bytes bytes) {
    char *room = ta_napi_writer_room(writer, bytes.length);
    if (!room) return;
    if (bytes.length) memcpy(room, bytes.pointer, bytes.length);
    ta_napi_written written = {TA_CBOR_BYTES, bytes.length};
    ta_napi_writer_string(writer, &written);
}

static void ta_napi_writer_simple(ta_napi_writer *writer, uint32_t simple) {
    ta_cbor_token token = {TA_CBOR_SIMPLE, 0, simple};
    ta_napi_writer_token(writer, &token);
}

/* A napi call that failed: an exception is pending (a getter threw), or
 * the value is not one Node-API reads. Either way the value is refused. */
static int ta_napi_writer_check(ta_napi_writer *writer, napi_status status) {
    if (status == napi_ok) return 1;
    if (writer->status == TA_OK) writer->status = TA_ERROR_INVALID;
    return 0;
}

/* A string's UTF-16 code units, read into scratch->utf16; how many. A
 * lone surrogate is refused later, by ta_napi_utf8: Node-API's UTF-8
 * conversion would write U+FFFD for it silently (bindings.md, section 9),
 * so the addon reads UTF-16 and encodes UTF-8 itself. */
static ta_status ta_napi_utf16(ta_napi_scratch *scratch, napi_value value, size_t *units) {
    size_t length = 0, read = 0;
    if (napi_get_value_string_utf16(scratch->env, value, NULL, 0, &length) != napi_ok) return TA_ERROR_INVALID;
    if (!ta_napi_buffer_reserve(&scratch->utf16, (length + 1) * sizeof(uint16_t))) return TA_ERROR_NO_MEMORY;
    if (napi_get_value_string_utf16(scratch->env, value, (char16_t *)scratch->utf16.pointer, length + 1, &read) !=
        napi_ok)
        return TA_ERROR_INVALID;
    *units = read;
    return TA_OK;
}

/* `count` UTF-16 units as UTF-8 at `out` (room for 3 bytes a unit). */
typedef struct ta_napi_utf8_out {
    const uint16_t *units;
    size_t count;
    char *out;
} ta_napi_utf8_out;

/* Its length, or TA_NIL for a lone surrogate. */
static size_t ta_napi_utf8(const ta_napi_utf8_out *conversion) {
    const uint16_t *utf16 = conversion->units;
    char *out = conversion->out;
    size_t length = 0, read = conversion->count;
    for (size_t i = 0; i < read; i++) {
        uint32_t c = utf16[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < read && utf16[i + 1] >= 0xDC00 && utf16[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (utf16[i + 1] - 0xDC00u);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            return TA_NIL; /* a lone surrogate */
        }
        if (c < 0x80) {
            out[length++] = (char)c;
        } else if (c < 0x800) {
            out[length++] = (char)(0xC0 | (c >> 6));
            out[length++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[length++] = (char)(0xE0 | (c >> 12));
            out[length++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[length++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[length++] = (char)(0xF0 | (c >> 18));
            out[length++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[length++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[length++] = (char)(0x80 | (c & 0x3F));
        }
    }
    return length;
}

/* A string as UTF-8 text; a lone surrogate is refused, never replaced. */
static void ta_napi_writer_text(ta_napi_writer *writer, napi_value value) {
    size_t units = 0;
    if (writer->status != TA_OK) return;
    ta_status status = ta_napi_utf16(writer->scratch, value, &units);
    if (status != TA_OK) {
        writer->status = status;
        return;
    }
    char *out = ta_napi_writer_room(writer, units * 3);
    if (!out) return;
    ta_napi_utf8_out conversion = {(const uint16_t *)writer->scratch->utf16.pointer, units, out};
    size_t length = ta_napi_utf8(&conversion);
    if (length == TA_NIL) {
        writer->status = TA_ERROR_INVALID;
        return;
    }
    ta_napi_written written = {TA_CBOR_TEXT, length};
    ta_napi_writer_string(writer, &written);
}

/* A number: an integer within ±(2^53-1) is an integer, any other number
 * (-0, NaN, ±Infinity, a fraction, a larger integer) a float. Never
 * napi_get_value_int64, which saturates silently. */
static void ta_napi_writer_number(ta_napi_writer *writer, double number) {
    uint64_t bits;
    memcpy(&bits, &number, sizeof bits);
    ta_cbor_token token = {TA_CBOR_FLOAT, 0, bits};
    if (number >= -TA_NAPI_SAFE_INTEGER && number <= TA_NAPI_SAFE_INTEGER && number == (double)(int64_t)number &&
        bits != (uint64_t)1 << 63) {
        int64_t integer = (int64_t)number;
        token.kind = integer >= 0 ? TA_CBOR_UINT : TA_CBOR_NEGINT;
        token.value = integer >= 0 ? (uint64_t)integer : (uint64_t)(-(integer + 1));
    }
    ta_napi_writer_token(writer, &token);
}

/* A BigInt within -2^64 … 2^64-1 is an integer; past it, not a value. */
static void ta_napi_writer_bigint(ta_napi_writer *writer, napi_value value) {
    int sign = 0;
    uint64_t words[3] = {0, 0, 0};
    size_t count = 3; /* in: room in `words`; out: the words it has */
    if (!ta_napi_writer_check(writer, napi_get_value_bigint_words(writer->env, value, &sign, &count, words))) return;
    if (count > 3) {
        writer->status = TA_ERROR_INVALID;
        return;
    }
    while (count > 1 && !words[count - 1])
        count--;
    ta_cbor_token token = {sign ? TA_CBOR_NEGINT : TA_CBOR_UINT, 0, 0};
    if (count <= 1 && !sign) {
        token.value = words[0];
    } else if (count <= 1 && sign && words[0]) {
        token.value = words[0] - 1;
    } else if (count == 2 && sign && !words[0] && words[1] == 1) {
        token.value = UINT64_MAX; /* -2^64 */
    } else {
        writer->status = TA_ERROR_INVALID;
        return;
    }
    ta_napi_writer_token(writer, &token);
}

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

/* The bytes of an ArrayBuffer, a typed array (its own bytes) or a
 * DataView, written; 0 when the value is none of them. */
static int ta_napi_writer_buffer(ta_napi_writer *writer, napi_value value) {
    ta_bytes bytes;
    if (!ta_napi_values_bytes(writer->env, value, &bytes)) return 0;
    ta_napi_writer_bytes(writer, bytes);
    return 1;
}

static int ta_napi_writer_value(ta_napi_writer *writer, napi_value value, uint32_t depth);

static void ta_napi_writer_array(ta_napi_writer *writer, napi_value value, uint32_t depth) {
    napi_env env = writer->env;
    uint32_t length = 0;
    if (!ta_napi_writer_check(writer, napi_get_array_length(env, value, &length))) return;
    ta_cbor_token token = {TA_CBOR_ARRAY, length, 0};
    ta_napi_writer_token(writer, &token);
    for (uint32_t i = 0; i < length && writer->status == TA_OK; i++) {
        napi_value item;
        if (!ta_napi_writer_check(writer, napi_get_element(env, value, i, &item))) return;
        if (!ta_napi_writer_value(writer, item, depth + 1)) ta_napi_writer_simple(writer, TA_CBOR_NULL);
    }
}

/* An object's own enumerable string keys, in the order Object.keys gives;
 * a property that is dropped (undefined, a function, a symbol) drops its
 * key too. */
static void ta_napi_writer_object(ta_napi_writer *writer, napi_value value, uint32_t depth) {
    napi_env env = writer->env;
    napi_value keys;
    uint32_t count = 0;
    if (!ta_napi_writer_check(writer, napi_get_all_property_names(env, value, napi_key_own_only,
                                                                  napi_key_enumerable | napi_key_skip_symbols,
                                                                  napi_key_numbers_to_strings, &keys)) ||
        !ta_napi_writer_check(writer, napi_get_array_length(env, keys, &count)))
        return;
    ta_cbor_token token = {TA_CBOR_MAP, 0, 0};
    uint32_t map = ta_napi_writer_token(writer, &token);
    for (uint32_t i = 0; i < count && writer->status == TA_OK; i++) {
        napi_value key, item;
        if (!ta_napi_writer_check(writer, napi_get_element(env, keys, i, &key)) ||
            !ta_napi_writer_check(writer, napi_get_property(env, value, key, &item)))
            return;
        uint32_t mark = writer->count;
        size_t strings = writer->length;
        ta_napi_writer_text(writer, key);
        int written = ta_napi_writer_value(writer, item, depth + 1);
        if (written == 0) {
            writer->count = mark;
            writer->length = strings;
        } else if (writer->status == TA_OK) {
            ((ta_cbor_token *)writer->scratch->tokens.pointer)[map].count++;
        }
    }
}

/* Write `value`: 1 when written, 0 when it is dropped (undefined, a
 * function, a symbol), -1 when it is refused (writer->status). */
static int ta_napi_writer_value(ta_napi_writer *writer, napi_value value, uint32_t depth) {
    napi_env env = writer->env;
    if (writer->status != TA_OK) return -1;
    if (depth > TA_CBOR_DEPTH_MAX) { /* a cycle, or nested too deep for a neutral value */
        writer->status = TA_ERROR_INVALID;
        return -1;
    }
    napi_valuetype type;
    if (!ta_napi_writer_check(writer, napi_typeof(env, value, &type))) return -1;
    switch (type) {
    case napi_undefined:
    case napi_symbol:
    case napi_function: return 0;
    case napi_null: ta_napi_writer_simple(writer, TA_CBOR_NULL); break;
    case napi_boolean: {
        bool b = false;
        if (ta_napi_writer_check(writer, napi_get_value_bool(env, value, &b)))
            ta_napi_writer_simple(writer, b ? TA_CBOR_TRUE : TA_CBOR_FALSE);
        break;
    }
    case napi_number: {
        double number = 0;
        if (ta_napi_writer_check(writer, napi_get_value_double(env, value, &number)))
            ta_napi_writer_number(writer, number);
        break;
    }
    case napi_bigint: ta_napi_writer_bigint(writer, value); break;
    case napi_string: ta_napi_writer_text(writer, value); break;
    case napi_object:
    case napi_external: {
        if (ta_napi_writer_buffer(writer, value)) break;
        napi_value to_json;
        napi_valuetype to_json_type = napi_undefined;
        if (type == napi_object) {
            if (!ta_napi_writer_check(writer, napi_get_named_property(env, value, "toJSON", &to_json)) ||
                !ta_napi_writer_check(writer, napi_typeof(env, to_json, &to_json_type)))
                return -1;
        }
        if (to_json_type == napi_function) {
            napi_value result;
            if (!ta_napi_writer_check(writer, napi_call_function(env, value, to_json, 0, NULL, &result))) return -1;
            return ta_napi_writer_value(writer, result, depth + 1);
        }
        bool array = false;
        if (!ta_napi_writer_check(writer, napi_is_array(env, value, &array))) return -1;
        if (array) ta_napi_writer_array(writer, value, depth);
        else ta_napi_writer_object(writer, value, depth);
        break;
    }
    default: writer->status = TA_ERROR_INVALID; break;
    }
    return writer->status == TA_OK ? 1 : -1;
}

ta_status ta_napi_values_encode(ta_napi_scratch *scratch, napi_value value, ta_bytes *out) {
    out->pointer = NULL;
    out->length = 0;
    if (scratch->writing) return TA_ERROR_STATE;
    ta_napi_writer writer = {scratch->env, scratch, 0, 0, TA_OK};
    scratch->writing = 1;
    if (!ta_napi_writer_value(&writer, value, 0)) ta_napi_writer_simple(&writer, TA_CBOR_NULL);
    scratch->writing = 0;
    ta_status status = writer.status;
    if (status != TA_OK) return status;
    ta_cbor_document document = {{scratch->strings.pointer, writer.length},
                                 (ta_cbor_token *)scratch->tokens.pointer,
                                 writer.count,
                                 writer.count};
    ta_buffer buffer = {scratch->out.pointer, scratch->out.capacity, 0};
    status = ta_cbor_encode(&document, &buffer);
    if (status == TA_ERROR_NO_MEMORY && ta_napi_buffer_reserve(&scratch->out, buffer.length)) {
        buffer = (ta_buffer){scratch->out.pointer, scratch->out.capacity, 0};
        status = ta_cbor_encode(&document, &buffer);
    }
    if (status == TA_OK) *out = (ta_bytes){scratch->out.pointer, buffer.length};
    return status;
}

/* ---- read: a message's values ------------------------------------------------- */

typedef struct ta_napi_reader {
    napi_env env;
    const ta_cbor_document *document;
    uint32_t at;
} ta_napi_reader;

/* -1 - n for n past what a number holds: a BigInt (n + 1 may be 2^64). */
static napi_status ta_napi_reader_negative(napi_env env, uint64_t n, napi_value *out) {
    uint64_t words[2] = {n + 1, n == UINT64_MAX ? 1 : 0};
    return napi_create_bigint_words(env, 1, n == UINT64_MAX ? 2 : 1, words, out);
}

/* The value at the reader's token, moving past it; TA_ERROR_INVALID when it
 * is not a neutral value. */
static ta_status ta_napi_reader_value(ta_napi_reader *reader, napi_value *out) {
    napi_env env = reader->env;
    const ta_cbor_document *document = reader->document;
    uint32_t at = reader->at++;
    const ta_cbor_token *token = &document->tokens[at];
    napi_status status = napi_ok;
    switch (token->kind) {
    case TA_CBOR_UINT:
        status = token->value <= TA_NAPI_SAFE_UINT ? napi_create_double(env, (double)token->value, out)
                                                   : napi_create_bigint_uint64(env, token->value, out);
        break;
    case TA_CBOR_NEGINT:
        status = token->value < TA_NAPI_SAFE_UINT ? napi_create_double(env, -1.0 - (double)token->value, out)
                                                  : ta_napi_reader_negative(env, token->value, out);
        break;
    case TA_CBOR_BYTES: {
        ta_string_view bytes = ta_cbor_string(document, at);
        void *data = NULL;
        napi_value buffer;
        status = napi_create_arraybuffer(env, bytes.length, &data, &buffer);
        if (status == napi_ok && bytes.length) memcpy(data, bytes.pointer, bytes.length);
        if (status == napi_ok) status = napi_create_typedarray(env, napi_uint8_array, bytes.length, buffer, 0, out);
        break;
    }
    case TA_CBOR_TEXT: {
        ta_string_view text = ta_cbor_string(document, at); /* UTF-8, checked by ta_cbor_decode */
        status = napi_create_string_utf8(env, text.pointer ? text.pointer : "", text.length, out);
        break;
    }
    case TA_CBOR_ARRAY: {
        status = napi_create_array_with_length(env, token->count, out);
        for (uint32_t i = 0; i < token->count && status == napi_ok; i++) {
            napi_value item;
            ta_status read = ta_napi_reader_value(reader, &item);
            if (read != TA_OK) return read;
            status = napi_set_element(env, *out, i, item);
        }
        break;
    }
    case TA_CBOR_MAP: {
        status = napi_create_object(env, out);
        for (uint32_t i = 0; i < token->count && status == napi_ok; i++) {
            if (document->tokens[reader->at].kind != TA_CBOR_TEXT) return TA_ERROR_INVALID; /* keys are text */
            napi_property_descriptor property;
            memset(&property, 0, sizeof property);
            ta_string_view key = ta_cbor_string(document, reader->at++);
            status = napi_create_string_utf8(env, key.pointer ? key.pointer : "", key.length, &property.name);
            ta_status read = status == napi_ok ? ta_napi_reader_value(reader, &property.value) : TA_OK;
            if (read != TA_OK) return read;
            /* defined, not set: a key "__proto__" is an ordinary key */
            property.attributes = napi_default_jsproperty;
            if (status == napi_ok) status = napi_define_properties(env, *out, 1, &property);
        }
        break;
    }
    case TA_CBOR_TAG: /* an actor reference: its address */
        if (token->value != TA_CBOR_TAG_URI || document->tokens[reader->at].kind != TA_CBOR_TEXT)
            return TA_ERROR_INVALID;
        return ta_napi_reader_value(reader, out);
    case TA_CBOR_SIMPLE:
        switch (token->value) {
        case TA_CBOR_FALSE: status = napi_get_boolean(env, false, out); break;
        case TA_CBOR_TRUE: status = napi_get_boolean(env, true, out); break;
        case TA_CBOR_NULL: status = napi_get_null(env, out); break;
        case TA_CBOR_UNDEFINED: status = napi_get_undefined(env, out); break;
        default: return TA_ERROR_INVALID;
        }
        break;
    case TA_CBOR_FLOAT: {
        double number;
        memcpy(&number, &token->value, sizeof number);
        status = napi_create_double(env, number, out);
        break;
    }
    default: return TA_ERROR_INVALID;
    }
    return status == napi_ok ? TA_OK : TA_ERROR_NO_MEMORY;
}

ta_status ta_napi_values_decode(ta_napi_scratch *scratch, ta_bytes cbor, napi_value *out) {
    ta_cbor_token small[64];
    ta_cbor_document document = {cbor, small, sizeof small / sizeof small[0], 0};
    ta_status status = ta_cbor_decode(&document);
    if (status == TA_ERROR_NO_MEMORY) {
        if (!ta_napi_buffer_reserve(&scratch->read_tokens, (size_t)document.count * sizeof(ta_cbor_token)))
            return TA_ERROR_NO_MEMORY;
        document.tokens = (ta_cbor_token *)scratch->read_tokens.pointer;
        document.capacity = document.count;
        status = ta_cbor_decode(&document);
    }
    if (status == TA_OK) {
        ta_napi_reader reader = {scratch->env, &document, 0};
        status = ta_napi_reader_value(&reader, out);
    }
    return status;
}

/* ---- the exports ------------------------------------------------------------- */

/* encode(value) -> Uint8Array: ta_cbor_encode of the value's tokens, on
 * the environment's scratch. */
napi_value ta_napi_encode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], result = NULL;
    napi_get_undefined(env, &argv[0]);
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
    ta_bytes cbor;
    ta_status status = ta_napi_values_encode(&ta_napi_module_of(env)->scratch, argv[0], &cbor);
    if (status != TA_OK) {
        ta_napi_throw_status(env, "encode", status);
        return NULL;
    }
    if (ta_napi_values_uint8array(env, cbor, &result) != napi_ok) return NULL;
    return result;
}

/* decode(bytes) -> value: one ta_cbor_decode of the bytes, borrowed, then
 * the values. */
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
    ta_status status = ta_napi_values_decode(&ta_napi_module_of(env)->scratch, cbor, &result);
    if (status != TA_OK) {
        ta_napi_throw_status(env, "decode", status);
        return NULL;
    }
    return result;
}

/* ---- for the API ----------------------------------------------------------------- */

ta_status ta_napi_values_texts(ta_napi_scratch *scratch, const ta_napi_texts *texts) {
    napi_env env = scratch->env;
    size_t total = 0;
    for (uint32_t i = 0; i < texts->count; i++) {
        napi_valuetype type = napi_undefined;
        size_t units = 0;
        texts->out[i].pointer = NULL;
        texts->out[i].length = 0;
        if (napi_typeof(env, texts->values[i], &type) != napi_ok) return TA_ERROR_INVALID;
        if (type == napi_undefined) continue;
        if (type != napi_string || napi_get_value_string_utf16(env, texts->values[i], NULL, 0, &units) != napi_ok)
            return TA_ERROR_INVALID;
        total += units * 3;
    }
    /* reserved at once: the views stay put while the next is written */
    if (!ta_napi_buffer_reserve(&scratch->text, total + 1)) return TA_ERROR_NO_MEMORY;
    size_t at = 0;
    for (uint32_t i = 0; i < texts->count; i++) {
        napi_valuetype type = napi_undefined;
        size_t units = 0;
        napi_typeof(env, texts->values[i], &type);
        if (type == napi_undefined) continue;
        ta_status status = ta_napi_utf16(scratch, texts->values[i], &units);
        if (status != TA_OK) return status;
        ta_napi_utf8_out conversion = {(const uint16_t *)scratch->utf16.pointer, units, scratch->text.pointer + at};
        size_t length = ta_napi_utf8(&conversion);
        if (length == TA_NIL) return TA_ERROR_INVALID;
        texts->out[i].pointer = scratch->text.pointer + at;
        texts->out[i].length = (uint32_t)length;
        at += length;
    }
    return TA_OK;
}

ta_status ta_napi_values_hold_text(ta_napi_scratch *scratch, napi_value value, ta_napi_held_text *out) {
    napi_valuetype type = napi_undefined;
    size_t units = 0;
    void *data = NULL;
    if (napi_typeof(scratch->env, value, &type) != napi_ok || type != napi_string) return TA_ERROR_INVALID;
    ta_status status = ta_napi_utf16(scratch, value, &units);
    if (status != TA_OK) return status;
    if (napi_create_arraybuffer(scratch->env, units * 3 + 1, &data, &out->holder) != napi_ok) return TA_ERROR_NO_MEMORY;
    ta_napi_utf8_out conversion = {(const uint16_t *)scratch->utf16.pointer, units, (char *)data};
    size_t length = ta_napi_utf8(&conversion);
    if (length == TA_NIL) return TA_ERROR_INVALID;
    out->text.pointer = (const char *)data;
    out->text.length = (uint32_t)length;
    return TA_OK;
}

napi_status ta_napi_values_uint8array(napi_env env, ta_bytes bytes, napi_value *out) {
    void *data = NULL;
    napi_value buffer;
    napi_status status = napi_create_arraybuffer(env, bytes.length, &data, &buffer);
    if (status == napi_ok && bytes.length) memcpy(data, bytes.pointer, bytes.length);
    return status == napi_ok ? napi_create_typedarray(env, napi_uint8_array, bytes.length, buffer, 0, out) : status;
}

/* Through the datamodel's from_wire: the value as CBOR, read by the
 * datamodel the host runs. */
ta_status ta_napi_values_payload(ta_napi_host *host, napi_value value, ta_payload *out) {
    napi_valuetype type = napi_undefined;
    *out = 0;
    if (napi_typeof(host->env, value, &type) != napi_ok || type == napi_undefined) return TA_OK;
    ta_bytes cbor;
    ta_status status = ta_napi_values_encode(&host->scratch, value, &cbor);
    if (status != TA_OK) return status;
    return host->binding.vtable->from_wire(host->binding.datamodel, cbor, out);
}

ta_status ta_napi_wire(ta_napi_scratch *scratch, const ta_napi_writing *writing, ta_bytes *out) {
    if (!scratch->wire.capacity && !ta_napi_buffer_reserve(&scratch->wire, 1024)) return TA_ERROR_NO_MEMORY;
    ta_buffer buffer = {scratch->wire.pointer, scratch->wire.capacity, 0};
    ta_status status = writing->write(writing->context, &buffer);
    if (status == TA_ERROR_NO_MEMORY && buffer.length > buffer.capacity &&
        ta_napi_buffer_reserve(&scratch->wire, buffer.length)) {
        buffer = (ta_buffer){scratch->wire.pointer, scratch->wire.capacity, 0};
        status = writing->write(writing->context, &buffer);
    }
    out->pointer = scratch->wire.pointer;
    out->length = status == TA_OK ? buffer.length : 0;
    return status;
}

/* A payload's to_wire into the host's wire buffer. */
typedef struct ta_napi_to_wire_call {
    const ta_napi_host *host;
    ta_payload payload;
} ta_napi_to_wire_call;

static ta_status ta_napi_to_wire_write(void *context, ta_buffer *out) {
    const ta_napi_to_wire_call *call = (const ta_napi_to_wire_call *)context;
    return call->host->binding.vtable->to_wire(call->host->binding.datamodel, call->payload, out);
}

/* Through the datamodel's to_wire, then the value table. */
ta_status ta_napi_values_of_payload(ta_napi_host *host, const ta_napi_payload_read *read, napi_value *out) {
    if (!read->payload || read->datamodel != 0)
        return napi_get_undefined(host->env, out) == napi_ok ? TA_OK : TA_ERROR_NO_MEMORY;
    ta_napi_to_wire_call call = {host, read->payload};
    ta_napi_writing writing = {ta_napi_to_wire_write, &call};
    ta_bytes cbor;
    ta_status status = ta_napi_wire(&host->scratch, &writing, &cbor);
    return status == TA_OK ? ta_napi_values_decode(&host->scratch, cbor, out) : status;
}
