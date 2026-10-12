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

#include <stdlib.h>
#include <string.h>

/* 2^53 - 1: the integers a number holds exactly. */
#define TA_NAPI_SAFE_INTEGER 9007199254740991.0
#define TA_NAPI_SAFE_UINT ((uint64_t)9007199254740991)

/* ---- written: a value's tokens ------------------------------------------------ */

/* Tokens and the text and bytes they point into, both grown with malloc. */
typedef struct ta_napi_writer {
    napi_env env;
    ta_cbor_token *tokens;
    uint32_t count, capacity;
    char *strings;
    size_t length, room;
    ta_status status;
    uint16_t *utf16; /* a string's UTF-16 code units, while it is written */
    size_t utf16_room;
} ta_napi_writer;

static void ta_napi_writer_free(ta_napi_writer *writer) {
    free(writer->tokens);
    free(writer->strings);
    free(writer->utf16);
}

/* Append a token; its index, or TA_NIL (and the status set). */
static uint32_t ta_napi_writer_token(ta_napi_writer *writer, const ta_cbor_token *token) {
    if (writer->status != TA_OK) return TA_NIL;
    if (writer->count == writer->capacity) {
        uint32_t capacity = writer->capacity ? writer->capacity * 2 : 64;
        ta_cbor_token *tokens = (ta_cbor_token *)realloc(writer->tokens, capacity * sizeof *tokens);
        if (!tokens) {
            writer->status = TA_ERROR_NO_MEMORY;
            return TA_NIL;
        }
        writer->tokens = tokens;
        writer->capacity = capacity;
    }
    writer->tokens[writer->count] = *token;
    return writer->count++;
}

/* Room for `more` bytes of text or bytes; a pointer to it, or NULL. */
static char *ta_napi_writer_room(ta_napi_writer *writer, size_t more) {
    if (writer->status != TA_OK) return NULL;
    if (more > UINT32_MAX) {
        writer->status = TA_ERROR_INVALID;
        return NULL;
    }
    if (!writer->strings || writer->room - writer->length < more) {
        size_t room = writer->room ? writer->room : 1024;
        while (room - writer->length < more)
            room *= 2;
        char *strings = (char *)realloc(writer->strings, room);
        if (!strings) {
            writer->status = TA_ERROR_NO_MEMORY;
            return NULL;
        }
        writer->strings = strings;
        writer->room = room;
    }
    return writer->strings + writer->length;
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

/* A string as UTF-8 text. A lone surrogate is refused, never replaced:
 * Node-API's UTF-8 conversion would write U+FFFD for it silently
 * (bindings.md, section 9), so the string is read as UTF-16 and encoded
 * here. */
static void ta_napi_writer_text(ta_napi_writer *writer, napi_value value) {
    napi_env env = writer->env;
    size_t units = 0;
    if (!ta_napi_writer_check(writer, napi_get_value_string_utf16(env, value, NULL, 0, &units))) return;
    if (writer->utf16_room < units + 1) {
        uint16_t *utf16 = (uint16_t *)realloc(writer->utf16, (units + 1) * sizeof *utf16);
        if (!utf16) {
            writer->status = TA_ERROR_NO_MEMORY;
            return;
        }
        writer->utf16 = utf16;
        writer->utf16_room = units + 1;
    }
    size_t read = 0;
    if (!ta_napi_writer_check(writer, napi_get_value_string_utf16(env, value, (char16_t *)writer->utf16,
                                                                  units + 1, &read)))
        return;
    char *out = ta_napi_writer_room(writer, units * 3); /* at most 3 bytes per unit */
    if (!out) return;
    size_t length = 0;
    const uint16_t *utf16 = writer->utf16;
    for (size_t i = 0; i < read; i++) {
        uint32_t c = utf16[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < read && utf16[i + 1] >= 0xDC00 && utf16[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (utf16[i + 1] - 0xDC00u);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            writer->status = TA_ERROR_INVALID; /* a lone surrogate */
            return;
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

/* The bytes of an ArrayBuffer, a typed array (its own bytes) or a
 * DataView; 0 when the value is none of them. */
static int ta_napi_writer_buffer(ta_napi_writer *writer, napi_value value) {
    napi_env env = writer->env;
    bool is = false;
    ta_bytes bytes = {NULL, 0};
    void *data = NULL;
    if (napi_is_arraybuffer(env, value, &is) == napi_ok && is) {
        size_t length = 0;
        if (!ta_napi_writer_check(writer, napi_get_arraybuffer_info(env, value, &data, &length))) return 1;
        bytes = (ta_bytes){data, length};
    } else if (napi_is_typedarray(env, value, &is) == napi_ok && is) {
        napi_typedarray_type type;
        size_t length = 0;
        if (!ta_napi_writer_check(writer, napi_get_typedarray_info(env, value, &type, &length, &data, NULL, NULL)))
            return 1;
        static const size_t sizes[] = {
            [napi_int8_array] = 1,      [napi_uint8_array] = 1,     [napi_uint8_clamped_array] = 1,
            [napi_int16_array] = 2,     [napi_uint16_array] = 2,    [napi_int32_array] = 4,
            [napi_uint32_array] = 4,    [napi_float32_array] = 4,   [napi_float64_array] = 8,
            [napi_bigint64_array] = 8,  [napi_biguint64_array] = 8,
        };
        size_t size = (size_t)type < sizeof sizes / sizeof sizes[0] && sizes[type] ? sizes[type] : 1;
        bytes = (ta_bytes){data, length * size};
    } else if (napi_is_dataview(env, value, &is) == napi_ok && is) {
        size_t length = 0;
        if (!ta_napi_writer_check(writer, napi_get_dataview_info(env, value, &length, &data, NULL, NULL))) return 1;
        bytes = (ta_bytes){data, length};
    } else {
        return 0;
    }
    if (!bytes.pointer) bytes.length = 0; /* detached */
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
            writer->tokens[map].count++;
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

ta_status ta_napi_values_encode(napi_env env, napi_value value, ta_buffer *out) {
    ta_napi_writer writer;
    memset(&writer, 0, sizeof writer);
    writer.env = env;
    out->pointer = NULL;
    out->capacity = out->length = 0;
    if (!ta_napi_writer_value(&writer, value, 0)) ta_napi_writer_simple(&writer, TA_CBOR_NULL);
    ta_status status = writer.status;
    if (status == TA_OK) {
        ta_cbor_document document = {{writer.strings, writer.length}, writer.tokens, writer.capacity, writer.count};
        ta_buffer buffer = {NULL, 0, 0};
        status = ta_cbor_encode(&document, &buffer); /* measures */
        if (status == TA_ERROR_NO_MEMORY) {
            buffer.pointer = malloc(buffer.length ? buffer.length : 1);
            buffer.capacity = buffer.length;
            status = buffer.pointer ? ta_cbor_encode(&document, &buffer) : TA_ERROR_NO_MEMORY;
        }
        if (status == TA_OK) *out = buffer;
        else free(buffer.pointer);
    }
    ta_napi_writer_free(&writer);
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

ta_status ta_napi_values_decode(napi_env env, ta_bytes cbor, napi_value *out) {
    ta_cbor_token small[64];
    ta_cbor_document document = {cbor, small, sizeof small / sizeof small[0], 0};
    ta_status status = ta_cbor_decode(&document);
    if (status == TA_ERROR_NO_MEMORY) {
        document.tokens = (ta_cbor_token *)malloc((size_t)document.count * sizeof *document.tokens);
        if (!document.tokens) return TA_ERROR_NO_MEMORY;
        document.capacity = document.count;
        status = ta_cbor_decode(&document);
    }
    if (status == TA_OK) {
        ta_napi_reader reader = {env, &document, 0};
        status = ta_napi_reader_value(&reader, out);
    }
    if (document.tokens != small) free(document.tokens);
    return status;
}

/* ---- the exports ------------------------------------------------------------- */

/* The bytes of an ArrayBuffer, a typed array or a DataView `value`; 0 when it
 * is none of them. */
int ta_napi_values_bytes(napi_env env, napi_value value, ta_bytes *out) {
    ta_napi_writer writer;
    memset(&writer, 0, sizeof writer);
    writer.env = env;
    int is = ta_napi_writer_buffer(&writer, value);
    if (is && writer.status == TA_OK) *out = (ta_bytes){writer.strings, writer.length};
    else free(writer.strings);
    free(writer.tokens);
    return is && writer.status == TA_OK;
}

/* encode(value) -> Uint8Array: ta_cbor_encode of the value's tokens. */
napi_value ta_napi_encode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], result = NULL;
    napi_get_undefined(env, &argv[0]);
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
    ta_buffer cbor;
    ta_status status = ta_napi_values_encode(env, argv[0], &cbor);
    if (status != TA_OK) {
        ta_napi_throw_status(env, status);
        return NULL;
    }
    void *data = NULL;
    napi_value buffer;
    if (napi_create_arraybuffer(env, cbor.length, &data, &buffer) == napi_ok) {
        if (cbor.length) memcpy(data, cbor.pointer, cbor.length);
        napi_create_typedarray(env, napi_uint8_array, cbor.length, buffer, 0, &result);
    }
    free(cbor.pointer);
    return result;
}

/* decode(bytes) -> value: one ta_cbor_decode, then the values. */
napi_value ta_napi_decode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], result = NULL;
    napi_get_undefined(env, &argv[0]);
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
    ta_bytes cbor;
    if (!ta_napi_values_bytes(env, argv[0], &cbor)) {
        ta_napi_throw_status(env, TA_ERROR_INVALID);
        return NULL;
    }
    ta_status status = ta_napi_values_decode(env, cbor, &result);
    free((void *)cbor.pointer);
    if (status != TA_OK) {
        ta_napi_throw_status(env, status);
        return NULL;
    }
    return result;
}
