/* cli/json.c — JSON text to one CBOR item and back (cli/json.h).
 *
 * Both walk the item with an explicit stack of the containers open, so
 * nothing recurses. Reading writes through the caller's ta_cbor_writer, one
 * item as it goes; writing reads with ta_cbor_reader and appends to the
 * caller's output, its numbers and strings through base/format. */
#include "cli/json.h"

#include "base/format.h"
#include "base/memory.h"
#include "cli/decimal.h"

/* ---- reading ------------------------------------------------------------------ */

typedef enum json_container {
    JSON_ARRAY = 1,
    JSON_OBJECT = 2
} json_container;

typedef enum json_constants {
    JSON_KEYS_INITIAL = 8, /* a key set's first slots */
    JSON_KEY_ALIGN = 8,
    JSON_BYTE_ALIGN = 1
} json_constants;

/* The keys an open object has read, as an open-addressed set of views of
 * their text (unescaped): a slot whose pointer is NULL is empty. */
typedef struct json_key_set {
    ta_string_view *slots;
    uint32_t capacity, count;
} json_key_set;

typedef struct json_frame {
    uint32_t kind; /* json_container */
    uint32_t pad;
    json_key_set keys; /* JSON_OBJECT's */
} json_frame;

typedef struct json_reader {
    ta_json_reading *reading;
    const unsigned char *text;
    size_t length, at;
    json_frame stack[TA_JSON_MAX_DEPTH];
    uint32_t depth;
    uint32_t pad;
    unsigned char *scratch; /* a value string's unescaped text, grown in the arena */
    size_t scratch_capacity;
} json_reader;

static ta_status json_fail(json_reader *reader, size_t offset, const char *reason) {
    reader->reading->failure.offset = offset;
    reader->reading->failure.reason = ta_string(reason);
    return TA_ERROR_INVALID;
}

static void json_skip_space(json_reader *reader) {
    while (reader->at < reader->length) {
        unsigned char byte = reader->text[reader->at];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') return;
        reader->at++;
    }
}

/* The length of the UTF-8 sequence at `at` (RFC 3629: no overlong form, no
 * surrogate, nothing past U+10FFFF), or 0 when there is none. */
static uint32_t json_utf8_length(const json_reader *reader, size_t at) {
    const unsigned char *text = reader->text + at;
    size_t room = reader->length - at;
    unsigned char first = text[0];
    if (first < 0x80) return 1;
    uint32_t length;
    unsigned char low = 0x80, high = 0xBF; /* the second byte's range */
    if (first >= 0xC2 && first <= 0xDF) {
        length = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
        length = 3;
        if (first == 0xE0) low = 0xA0;
        if (first == 0xED) high = 0x9F; /* not a surrogate */
    } else if (first >= 0xF0 && first <= 0xF4) {
        length = 4;
        if (first == 0xF0) low = 0x90;
        if (first == 0xF4) high = 0x8F;
    } else {
        return 0;
    }
    if (room < length || text[1] < low || text[1] > high) return 0;
    for (uint32_t i = 2; i < length; i++)
        if (text[i] < 0x80 || text[i] > 0xBF) return 0;
    return length;
}

static int json_hex_value(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

/* The four hexadecimal digits of a \u escape at `at` (its backslash):
 * the code unit, or -1. */
static int32_t json_code_unit(const json_reader *reader, size_t at) {
    if (reader->length - at < 6 || reader->text[at] != '\\' || reader->text[at + 1] != 'u') return -1;
    int32_t unit = 0;
    for (size_t i = at + 2; i < at + 6; i++) {
        int digit = json_hex_value(reader->text[i]);
        if (digit < 0) return -1;
        unit = unit * 16 + digit;
    }
    return unit;
}

/* A code point as UTF-8 at `out`: the bytes written. */
static uint32_t json_utf8_encode(uint32_t point, unsigned char *out) {
    if (point < 0x80) {
        out[0] = (unsigned char)point;
        return 1;
    }
    if (point < 0x800) {
        out[0] = (unsigned char)(0xC0 | (point >> 6));
        out[1] = (unsigned char)(0x80 | (point & 0x3F));
        return 2;
    }
    if (point < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (point >> 12));
        out[1] = (unsigned char)(0x80 | ((point >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (point & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | (point >> 18));
    out[1] = (unsigned char)(0x80 | ((point >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((point >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (point & 0x3F));
    return 4;
}

/* Where a string's text goes once read: a key is kept for its object's
 * set, a value only until it is written. */
typedef enum json_string_use {
    JSON_STRING_VALUE = 0,
    JSON_STRING_KEY = 1
} json_string_use;

/* The string at reader->at (its opening quote) as text: a view of the JSON
 * text when it has no escape, else its unescaped bytes in the arena. The
 * reader is left after the closing quote. */
static ta_status json_string(json_reader *reader, uint32_t use, ta_string_view *out) {
    size_t start = reader->at + 1, end = start;
    int escaped = 0;
    /* first pass: where it ends, every byte checked but the escapes' */
    for (;;) {
        if (end >= reader->length) return json_fail(reader, reader->at, "a string that does not end");
        unsigned char byte = reader->text[end];
        if (byte == '"') break;
        if (byte < 0x20) return json_fail(reader, end, "a control character in a string");
        if (byte == '\\') {
            escaped = 1;
            end += 2;
            continue;
        }
        uint32_t length = json_utf8_length(reader, end);
        if (!length) return json_fail(reader, end, "text that is not UTF-8");
        end += length;
    }
    reader->at = end + 1;
    if (!escaped) {
        out->pointer = (const char *)reader->text + start;
        out->length = (uint32_t)(end - start);
        return TA_OK;
    }
    /* second pass: the escapes; the text never grows by unescaping */
    size_t room = end - start;
    unsigned char *bytes;
    if (use == JSON_STRING_KEY) {
        bytes = (unsigned char *)ta_growable_arena_alloc(reader->reading->arena, room, JSON_BYTE_ALIGN);
    } else {
        if (room > reader->scratch_capacity) {
            size_t capacity = reader->scratch_capacity * 2 > room ? reader->scratch_capacity * 2 : room;
            reader->scratch =
                (unsigned char *)ta_growable_arena_alloc(reader->reading->arena, capacity, JSON_BYTE_ALIGN);
            reader->scratch_capacity = reader->scratch ? capacity : 0;
        }
        bytes = reader->scratch;
    }
    if (!bytes) return TA_ERROR_NO_MEMORY;
    size_t written = 0;
    for (size_t at = start; at < end;) {
        unsigned char byte = reader->text[at];
        if (byte != '\\') {
            bytes[written++] = byte;
            at++;
            continue;
        }
        unsigned char next = reader->text[at + 1];
        static const char escapes[] = "\"\\/bfnrt";
        static const char meanings[] = "\"\\/\b\f\n\r\t";
        uint32_t found = 0;
        while (escapes[found] && (unsigned char)escapes[found] != next)
            found++;
        if (escapes[found]) {
            bytes[written++] = (unsigned char)meanings[found];
            at += 2;
            continue;
        }
        if (next != 'u') return json_fail(reader, at, "an escape JSON does not have");
        int32_t unit = json_code_unit(reader, at);
        if (unit < 0) return json_fail(reader, at, "an escape JSON does not have");
        uint32_t point = (uint32_t)unit;
        if (unit >= 0xDC00 && unit <= 0xDFFF) return json_fail(reader, at, "a lone surrogate");
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            int32_t low = at + 6 < end ? json_code_unit(reader, at + 6) : -1;
            if (low < 0xDC00 || low > 0xDFFF) return json_fail(reader, at, "a lone surrogate");
            point = 0x10000 + (((uint32_t)unit - 0xD800) << 10) + ((uint32_t)low - 0xDC00);
            at += 6;
        }
        at += 6;
        written += json_utf8_encode(point, bytes + written);
    }
    out->pointer = (const char *)bytes;
    out->length = (uint32_t)written;
    return TA_OK;
}

/* FNV-1a over a key's text. */
static uint32_t json_key_hash(ta_string_view key) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < key.length; i++)
        hash = (hash ^ (unsigned char)key.pointer[i]) * 16777619u;
    return hash;
}

static int json_same_key(ta_string_view a, ta_string_view b) {
    return a.length == b.length && ta_memcmp(a.pointer, b.pointer, a.length) == 0;
}

/* Put `key` in the set, at the first free slot of its probe. */
static void json_key_place(json_key_set *keys, ta_string_view key) {
    uint32_t mask = keys->capacity - 1;
    uint32_t slot = json_key_hash(key) & mask;
    while (keys->slots[slot].pointer)
        slot = (slot + 1) & mask;
    keys->slots[slot] = key;
    keys->count++;
}

/* Add a key to the object's set: TA_OK; TA_ERROR_INVALID when it is there
 * already; TA_ERROR_NO_MEMORY. The set grows at three quarters full. */
static ta_status json_key_add(json_reader *reader, json_key_set *keys, ta_string_view key) {
    if ((keys->count + 1) * 4 > keys->capacity * 3) {
        uint32_t capacity = keys->capacity ? keys->capacity * 2 : JSON_KEYS_INITIAL;
        ta_string_view *slots = (ta_string_view *)ta_growable_arena_alloc(
            reader->reading->arena, (size_t)capacity * sizeof *slots, JSON_KEY_ALIGN);
        if (!slots) return TA_ERROR_NO_MEMORY;
        ta_memset(slots, 0, (size_t)capacity * sizeof *slots);
        json_key_set grown = {slots, capacity, 0};
        for (uint32_t i = 0; i < keys->capacity; i++)
            if (keys->slots[i].pointer) json_key_place(&grown, keys->slots[i]);
        *keys = grown; /* the old slots stay in the arena until the caller releases it */
    }
    uint32_t mask = keys->capacity - 1;
    for (uint32_t slot = json_key_hash(key) & mask; keys->slots[slot].pointer; slot = (slot + 1) & mask)
        if (json_same_key(keys->slots[slot], key)) return TA_ERROR_INVALID;
    json_key_place(keys, key);
    return TA_OK;
}

/* A number's parts, found while its grammar is checked:
 * -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
typedef struct json_number {
    ta_string_view text;
    size_t integer_start, integer_end, fraction_start, fraction_end;
    int64_t exponent; /* past 10000 in size, a value as far: it reads the same */
    uint32_t negative;
    uint32_t pad;
} json_number;

static int json_is_digit(unsigned char byte) {
    return byte >= '0' && byte <= '9';
}

/* The number at reader->at: its parts, the reader left after it. */
static ta_status json_number_scan(json_reader *reader, json_number *number) {
    size_t start = reader->at, at = start;
    const unsigned char *text = reader->text;
    ta_memset(number, 0, sizeof *number);
    if (text[at] == '-') {
        number->negative = 1;
        at++;
    }
    if (at >= reader->length || !json_is_digit(text[at])) return json_fail(reader, at, "a number without digits");
    number->integer_start = at;
    if (text[at++] != '0')
        while (at < reader->length && json_is_digit(text[at]))
            at++;
    number->integer_end = number->fraction_start = number->fraction_end = at;
    if (at < reader->length && text[at] == '.') {
        at++;
        if (at >= reader->length || !json_is_digit(text[at]))
            return json_fail(reader, at, "a number without digits after its point");
        number->fraction_start = at;
        while (at < reader->length && json_is_digit(text[at]))
            at++;
        number->fraction_end = at;
    }
    if (at < reader->length && (text[at] == 'e' || text[at] == 'E')) {
        at++;
        int64_t sign = 1;
        if (at < reader->length && (text[at] == '+' || text[at] == '-')) sign = text[at++] == '-' ? -1 : 1;
        if (at >= reader->length || !json_is_digit(text[at]))
            return json_fail(reader, at, "a number without digits in its exponent");
        for (; at < reader->length && json_is_digit(text[at]); at++)
            if (number->exponent < 10000) number->exponent = number->exponent * 10 + (text[at] - '0');
        number->exponent *= sign;
    }
    number->text = (ta_string_view){(const char *)text + start, (uint32_t)(at - start)};
    reader->at = at;
    return TA_OK;
}

/* The i-th digit of the number's integer and fraction digits, one run. */
static unsigned char json_number_digit(const json_number *number, const unsigned char *text, size_t i) {
    size_t integer = number->integer_end - number->integer_start;
    return i < integer ? text[number->integer_start + i] : text[number->fraction_start + i - integer];
}

/* The number as CBOR: an integer when its value is one in range, else a
 * float (refused when it rounds past the largest double). */
static ta_status json_number_write(json_reader *reader, size_t start) {
    json_number number;
    ta_status status = json_number_scan(reader, &number);
    if (status != TA_OK) return status;
    const unsigned char *text = reader->text;
    size_t digits = (number.integer_end - number.integer_start) + (number.fraction_end - number.fraction_start);
    size_t first = 0, last = digits; /* the first and last digits that are not 0 */
    while (first < digits && json_number_digit(&number, text, first) == '0')
        first++;
    while (last > first && json_number_digit(&number, text, last - 1) == '0')
        last--;
    /* value = digits[first, last) × 10^scale */
    int64_t scale = number.exponent - (int64_t)(number.fraction_end - number.fraction_start) +
                    (int64_t)(digits - last);
    if (first == digits && !number.negative) { /* 0, 0.0, 0e9 */
        ta_cbor_write_uint(reader->reading->writer, 0);
        return TA_OK;
    }
    int64_t magnitude_digits = (int64_t)(last - first) + scale;
    if (first < digits && scale >= 0 && magnitude_digits <= 20) {
        /* an integer: within -2^64 … 2^64-1 unless 20 digits past the limit */
        static const char largest[] = "18446744073709551615", lowest[] = "18446744073709551616";
        const char *limit = number.negative ? lowest : largest;
        int fits = 1;
        if (magnitude_digits == 20) {
            for (int64_t i = 0; i < 20; i++) {
                unsigned char digit =
                    i < (int64_t)(last - first) ? json_number_digit(&number, text, first + (size_t)i) : '0';
                if (digit != (unsigned char)limit[i]) {
                    fits = digit < (unsigned char)limit[i];
                    break;
                }
            }
        }
        if (fits) {
            uint64_t magnitude = 0; /* 2^64 itself wraps to 0, and -2^64 is -1 - (2^64 - 1) */
            for (int64_t i = 0; i < magnitude_digits; i++)
                magnitude = magnitude * 10 +
                            (i < (int64_t)(last - first) ? (uint64_t)(json_number_digit(&number, text, first + (size_t)i) - '0') : 0);
            if (number.negative) ta_cbor_write_negative(reader->reading->writer, magnitude - 1);
            else ta_cbor_write_uint(reader->reading->writer, magnitude);
            return TA_OK;
        }
    }
    ta_decimal_reading decimal = {number.text, 0};
    if (ta_decimal_read(&decimal) != TA_OK) return json_fail(reader, start, "a number out of range");
    ta_cbor_write_float(reader->reading->writer, decimal.value);
    return TA_OK;
}

/* true, false or null at reader->at. */
static ta_status json_literal(json_reader *reader) {
    static const struct {
        const char *text;
        uint32_t simple;
    } literals[] = {{"true", TA_CBOR_TRUE}, {"false", TA_CBOR_FALSE}, {"null", TA_CBOR_NULL}};
    for (uint32_t i = 0; i < sizeof literals / sizeof literals[0]; i++) {
        ta_string_view word = ta_string(literals[i].text);
        if (reader->length - reader->at >= word.length &&
            ta_memcmp(reader->text + reader->at, word.pointer, word.length) == 0) {
            reader->at += word.length;
            ta_cbor_write_simple(reader->reading->writer, literals[i].simple);
            return TA_OK;
        }
    }
    return json_fail(reader, reader->at, "not a JSON value");
}

/* An object's key at reader->at (after the space before it), and the colon
 * after it. */
static ta_status json_key(json_reader *reader) {
    if (reader->at >= reader->length || reader->text[reader->at] != '"')
        return json_fail(reader, reader->at, "an object's key that is not a string");
    size_t start = reader->at;
    ta_string_view key;
    ta_status status = json_string(reader, JSON_STRING_KEY, &key);
    if (status != TA_OK) return status;
    status = json_key_add(reader, &reader->stack[reader->depth - 1].keys, key);
    if (status == TA_ERROR_INVALID) return json_fail(reader, start, "a key repeated in an object");
    if (status != TA_OK) return status;
    ta_cbor_write_text(reader->reading->writer, key);
    json_skip_space(reader);
    if (reader->at >= reader->length || reader->text[reader->at] != ':')
        return json_fail(reader, reader->at, "an object's key without a colon");
    reader->at++;
    return TA_OK;
}

/* Open an array or an object; TA_ERROR_LIMIT past TA_JSON_MAX_DEPTH. */
static ta_status json_open(json_reader *reader, uint32_t kind) {
    if (reader->depth == TA_JSON_MAX_DEPTH) {
        reader->reading->failure.offset = reader->at;
        reader->reading->failure.reason = ta_string("nested deeper than 64");
        return TA_ERROR_LIMIT;
    }
    json_frame *frame = &reader->stack[reader->depth++];
    ta_memset(frame, 0, sizeof *frame);
    frame->kind = kind;
    if (kind == JSON_OBJECT) ta_cbor_write_map(reader->reading->writer, TA_CBOR_COUNT_LATER);
    else ta_cbor_write_array(reader->reading->writer, TA_CBOR_COUNT_LATER);
    reader->at++;
    return TA_OK;
}

/* One value at reader->at; a container is opened, not read (*opened 1). */
static ta_status json_value(json_reader *reader, int *opened) {
    *opened = 0;
    if (reader->at >= reader->length) return json_fail(reader, reader->at, "the text ends before a value");
    unsigned char byte = reader->text[reader->at];
    if (byte == '{' || byte == '[') {
        *opened = 1;
        return json_open(reader, byte == '{' ? JSON_OBJECT : JSON_ARRAY);
    }
    if (byte == '"') {
        ta_string_view text;
        ta_status status = json_string(reader, JSON_STRING_VALUE, &text);
        if (status == TA_OK) ta_cbor_write_text(reader->reading->writer, text);
        return status;
    }
    if (byte == '-' || json_is_digit(byte)) return json_number_write(reader, reader->at);
    return json_literal(reader);
}

/* The innermost container ends at reader->at: 1 when it did (and is
 * closed), 0 when it does not end here. */
static int json_close(json_reader *reader) {
    const json_frame *frame = &reader->stack[reader->depth - 1];
    unsigned char closer = frame->kind == JSON_OBJECT ? '}' : ']';
    if (reader->at >= reader->length || reader->text[reader->at] != closer) return 0;
    reader->at++;
    reader->depth--;
    ta_cbor_write_end(reader->reading->writer);
    return 1;
}

ta_status ta_json_read(ta_json_reading *reading) {
    json_reader reader;
    reader.reading = reading;
    reader.text = (const unsigned char *)reading->text.pointer;
    reader.length = reading->text.length;
    reader.at = 0;
    reader.depth = 0;
    reader.scratch = NULL;
    reader.scratch_capacity = 0;
    reading->failure.offset = 0;
    reading->failure.reason = ta_string("");
    int want_value = 1; /* a value comes next; else a comma or a container's end */
    for (;;) {
        json_skip_space(&reader);
        if (want_value) {
            int opened;
            ta_status status = json_value(&reader, &opened);
            if (status != TA_OK) return status;
            want_value = 0;
            if (!opened) continue;
            /* a container just opened: its end at once, or its first member */
            json_skip_space(&reader);
            if (json_close(&reader)) continue;
            if (reader.stack[reader.depth - 1].kind == JSON_OBJECT) {
                status = json_key(&reader);
                if (status != TA_OK) return status;
            }
            want_value = 1;
            continue;
        }
        if (!reader.depth) break;
        if (json_close(&reader)) continue;
        uint32_t kind = reader.stack[reader.depth - 1].kind;
        if (reader.at >= reader.length || reader.text[reader.at] != ',')
            return json_fail(&reader, reader.at,
                             kind == JSON_OBJECT ? "an object without a comma or its end"
                                                 : "an array without a comma or its end");
        reader.at++;
        json_skip_space(&reader);
        if (kind == JSON_OBJECT) {
            ta_status status = json_key(&reader);
            if (status != TA_OK) return status;
        }
        want_value = 1;
    }
    if (reader.at != reader.length) return json_fail(&reader, reader.at, "text after the value");
    return reading->writer->status; /* TA_OK, or TA_ERROR_NO_MEMORY: it could not grow */
}

/* ---- writing ------------------------------------------------------------------ */

typedef struct json_out_frame {
    uint64_t remaining; /* items still to come */
    uint32_t kind;      /* TA_CBOR_ARRAY, TA_CBOR_MAP or TA_CBOR_TAG */
    uint32_t position;  /* the next item's place: in a map, keys at even places */
} json_out_frame;

static void json_put(ta_output *out, const char *text) {
    ta_string_view view = ta_string(text);
    ta_output_write(out, view.pointer, view.length);
}

static void json_put_unsigned(ta_output *out, uint64_t value) {
    char digits[TA_FORMAT_U64_MAX];
    ta_output_write(out, digits, ta_format_u64(digits, value));
}

/* Bytes as base64url text without padding (RFC 4648, section 5), quoted. */
static void json_put_base64url(ta_output *out, ta_bytes bytes) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const unsigned char *data = (const unsigned char *)bytes.pointer;
    ta_output_write(out, "\"", 1);
    for (size_t at = 0; at < bytes.length; at += 3) {
        size_t taken = bytes.length - at < 3 ? bytes.length - at : 3;
        uint32_t group = (uint32_t)data[at] << 16;
        if (taken > 1) group |= (uint32_t)data[at + 1] << 8;
        if (taken > 2) group |= data[at + 2];
        char quad[4];
        for (uint32_t i = 0; i < 4; i++)
            quad[i] = alphabet[(group >> (18 - 6 * i)) & 0x3F];
        ta_output_write(out, quad, taken + 1);
    }
    ta_output_write(out, "\"", 1);
}

static double json_double(uint64_t bits) {
    double value;
    ta_memcpy(&value, &bits, sizeof value);
    return value;
}

/* One scalar item. */
static void json_put_scalar(ta_json_writing *writing, const ta_cbor_item *item) {
    ta_output *out = writing->out;
    switch (item->kind) {
    case TA_CBOR_UINT: json_put_unsigned(out, item->value); break;
    case TA_CBOR_NEGINT:
        ta_output_write(out, "-", 1);
        if (item->value == UINT64_MAX) json_put(out, "18446744073709551616"); /* -2^64 */
        else json_put_unsigned(out, item->value + 1);
        break;
    case TA_CBOR_TEXT:
        ta_format_quoted(out, (ta_string_view){(const char *)item->bytes.pointer, (uint32_t)item->bytes.length});
        break;
    case TA_CBOR_BYTES:
        json_put_base64url(out, item->bytes);
        writing->changes |= TA_JSON_CHANGED_BYTES;
        break;
    case TA_CBOR_FLOAT:
        if (((item->value >> 52) & 0x7FF) == 0x7FF) { /* NaN, an infinity */
            json_put(out, "null");
            writing->changes |= TA_JSON_CHANGED_FLOAT;
        } else {
            ta_format_double(out, json_double(item->value));
        }
        break;
    default: /* a simple value */
        if (item->value == TA_CBOR_FALSE) json_put(out, "false");
        else if (item->value == TA_CBOR_TRUE) json_put(out, "true");
        else {
            json_put(out, "null");
            if (item->value != TA_CBOR_NULL) writing->changes |= TA_JSON_CHANGED_UNDEFINED;
        }
        break;
    }
}

ta_status ta_json_write(ta_json_writing *writing) {
    ta_cbor_reader reader;
    json_out_frame stack[TA_CBOR_DEPTH_MAX + 1];
    uint32_t depth = 0;
    ta_output *out = writing->out;
    writing->changes = 0;
    ta_cbor_reader_init(&reader, writing->cbor);
    for (;;) {
        while (depth && stack[depth - 1].remaining == 0) { /* containers the last item completed */
            depth--;
            if (stack[depth].kind == TA_CBOR_ARRAY) ta_output_write(out, "]", 1);
            else if (stack[depth].kind == TA_CBOR_MAP) ta_output_write(out, "}", 1);
        }
        ta_cbor_item item;
        int32_t read = ta_cbor_read(&reader, &item);
        if (read < 0) return (ta_status)read;
        if (read == 0) break;
        if (depth) {
            json_out_frame *parent = &stack[depth - 1];
            uint32_t position = parent->position++;
            parent->remaining--;
            if (parent->kind == TA_CBOR_MAP) {
                if (position % 2 == 0 && item.kind != TA_CBOR_TEXT) return TA_ERROR_UNSUPPORTED;
                if (position) ta_output_write(out, position % 2 ? ":" : ",", 1);
            } else if (parent->kind == TA_CBOR_ARRAY && position) {
                ta_output_write(out, ",", 1);
            }
        }
        if (item.kind == TA_CBOR_ARRAY || item.kind == TA_CBOR_MAP || item.kind == TA_CBOR_TAG) {
            json_out_frame *frame = &stack[depth++];
            frame->kind = item.kind;
            frame->position = 0;
            frame->remaining = item.kind == TA_CBOR_TAG   ? 1u
                               : item.kind == TA_CBOR_MAP ? (uint64_t)item.count * 2u
                                                          : item.count;
            if (item.kind == TA_CBOR_ARRAY) ta_output_write(out, "[", 1);
            else if (item.kind == TA_CBOR_MAP) ta_output_write(out, "{", 1);
            else writing->changes |= TA_JSON_CHANGED_TAG;
            continue;
        }
        json_put_scalar(writing, &item);
    }
    return ta_output_fits(out) ? TA_OK : TA_ERROR_NO_MEMORY;
}

/* ---- base64url ---------------------------------------------------------------- */

/* A character's six bits in base64url's alphabet, or -1. */
static int json_base64url_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

ta_status ta_json_base64url_read(ta_string_view text, ta_buffer *out) {
    size_t tail = text.length % 4;
    if (tail == 1) return TA_ERROR_INVALID;
    size_t needed = (size_t)text.length / 4 * 3 + (tail ? tail - 1 : 0);
    for (uint32_t i = 0; i < text.length; i++)
        if (json_base64url_value(text.pointer[i]) < 0) return TA_ERROR_INVALID;
    out->length = needed;
    if (needed > out->capacity) return TA_ERROR_NO_MEMORY;
    unsigned char *bytes = (unsigned char *)out->pointer;
    size_t written = 0;
    for (uint32_t at = 0; at < text.length; at += 4) {
        uint32_t digits = text.length - at < 4 ? text.length - at : 4;
        uint32_t group = 0;
        for (uint32_t i = 0; i < 4; i++)
            group = group << 6 | (i < digits ? (uint32_t)json_base64url_value(text.pointer[at + i]) : 0u);
        for (uint32_t i = 0; i + 1 < digits; i++)
            bytes[written++] = (unsigned char)(group >> (16 - 8 * i));
    }
    return TA_OK;
}
