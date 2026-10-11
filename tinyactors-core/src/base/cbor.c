/* cbor.c — CBOR (RFC 8949) to and from token lists (include/tinyactors/cbor.h).
 *
 * Both directions walk the item tree with an explicit stack of how many
 * items each open container still has, so nothing recurses. */
#include <tinyactors/cbor.h>

#include "base/memory.h"

enum {
    MAJOR_UINT = 0,
    MAJOR_NEGINT = 1,
    MAJOR_BYTES = 2,
    MAJOR_TEXT = 3,
    MAJOR_ARRAY = 4,
    MAJOR_MAP = 5,
    MAJOR_TAG = 6,
    MAJOR_SIMPLE = 7
};

enum { INFO_ONE_BYTE = 24, INFO_TWO_BYTES = 25, INFO_FOUR_BYTES = 26, INFO_EIGHT_BYTES = 27, INFO_INDEFINITE = 31 };

/* ---- the stack of open containers ------------------------------------------- */

/* For each open container or tag, the items it still has to see. */
typedef struct cbor_nesting {
    uint64_t remaining[TA_CBOR_DEPTH_MAX];
    uint32_t depth;
} cbor_nesting;

/* An item with `items` children starts: open it, unless it has none. */
static ta_status nesting_open(cbor_nesting *nesting, uint64_t items) {
    if (nesting->depth == TA_CBOR_DEPTH_MAX) return TA_ERROR_INVALID;
    nesting->remaining[nesting->depth++] = items;
    return TA_OK;
}

/* An item is complete: count it in its container, and close every container
 * it completes. Returns 1 when the top-level item is complete. */
static int nesting_complete(cbor_nesting *nesting) {
    while (nesting->depth) {
        if (--nesting->remaining[nesting->depth - 1]) return 0;
        nesting->depth--;
    }
    return 1;
}

/* ---- UTF-8 ------------------------------------------------------------------- */

/* Is it well-formed UTF-8 (no overlong forms, surrogates or code points past
 * U+10FFFF)? */
static int utf8_valid(const unsigned char *text, size_t length) {
    size_t at = 0;
    while (at < length) {
        unsigned char lead = text[at];
        if (lead < 0x80) {
            at++;
            continue;
        }
        size_t more;
        unsigned char low = 0x80, high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) more = 1;
        else if (lead == 0xE0) more = 2, low = 0xA0;
        else if (lead == 0xED) more = 2, high = 0x9F;
        else if (lead >= 0xE1 && lead <= 0xEF) more = 2;
        else if (lead == 0xF0) more = 3, low = 0x90;
        else if (lead == 0xF4) more = 3, high = 0x8F;
        else if (lead >= 0xF1 && lead <= 0xF3) more = 3;
        else return 0;
        if (length - at - 1 < more) return 0;
        if (text[at + 1] < low || text[at + 1] > high) return 0;
        for (size_t k = 2; k <= more; k++)
            if ((text[at + k] & 0xC0) != 0x80) return 0;
        at += more + 1;
    }
    return 1;
}

/* ---- floats ------------------------------------------------------------------ */

static uint64_t double_bits(double value) {
    uint64_t bits;
    ta_memcpy(&bits, &value, sizeof bits);
    return bits;
}

static double bits_double(uint64_t bits) {
    double value;
    ta_memcpy(&value, &bits, sizeof value);
    return value;
}

/* A half-precision float's bits as a double's. */
static uint64_t half_widen(uint32_t half) {
    uint64_t sign = (uint64_t)(half >> 15) << 63;
    uint32_t exponent = (half >> 10) & 0x1F, mantissa = half & 0x3FF;
    if (exponent == 0x1F) return sign | UINT64_C(0x7FF0000000000000) | (uint64_t)mantissa << 42;
    if (exponent == 0) {
        if (!mantissa) return sign;
        /* subnormal: mantissa * 2^-24; normalize */
        int32_t shift = 0;
        while (!(mantissa & 0x400)) {
            mantissa <<= 1;
            shift++;
        }
        return sign | (uint64_t)(1023 - 14 - shift) << 52 | (uint64_t)(mantissa & 0x3FF) << 42;
    }
    return sign | (uint64_t)(exponent - 15 + 1023) << 52 | (uint64_t)mantissa << 42;
}

/* A single-precision float's bits as a double's. */
static uint64_t single_widen(uint32_t single) {
    float value;
    ta_memcpy(&value, &single, sizeof value);
    return double_bits((double)value);
}

/* The double `bits` as a half, if a half holds it exactly. */
static int half_narrow(uint64_t bits, uint32_t *half) {
    uint32_t sign = (uint32_t)(bits >> 63) << 15;
    int32_t exponent = (int32_t)((bits >> 52) & 0x7FF);
    uint64_t mantissa = bits & UINT64_C(0xFFFFFFFFFFFFF);
    if (exponent == 0x7FF) {
        if (mantissa) return 0; /* NaN: the caller writes the canonical one */
        *half = sign | 0x7C00;
        return 1;
    }
    if (exponent == 0) {
        if (mantissa) return 0; /* a double subnormal is far below a half's range */
        *half = sign;
        return 1;
    }
    int32_t power = exponent - 1023;
    if (power > 15 || power < -24) return 0;
    if (power >= -14) {
        if (mantissa & ((UINT64_C(1) << 42) - 1)) return 0;
        *half = sign | (uint32_t)(power + 15) << 10 | (uint32_t)(mantissa >> 42);
        return 1;
    }
    /* a half subnormal: (2^52 + mantissa) * 2^(power-52) = m * 2^-24 */
    uint64_t full = (UINT64_C(1) << 52) | mantissa;
    uint32_t shift = (uint32_t)(28 - power);
    if (full & ((UINT64_C(1) << shift) - 1)) return 0;
    *half = sign | (uint32_t)(full >> shift);
    return 1;
}

/* ---- decoding ---------------------------------------------------------------- */

typedef struct cbor_reader {
    const unsigned char *bytes;
    size_t length, at;
} cbor_reader;

/* The argument of a head with additional information `info`. */
static ta_status read_argument(cbor_reader *reader, uint32_t info, uint64_t *argument) {
    if (info < INFO_ONE_BYTE) {
        *argument = info;
        return TA_OK;
    }
    if (info > INFO_EIGHT_BYTES) return TA_ERROR_INVALID;
    size_t width = (size_t)1 << (info - INFO_ONE_BYTE);
    if (reader->length - reader->at < width) return TA_ERROR_INVALID;
    uint64_t value = 0;
    for (size_t k = 0; k < width; k++)
        value = value << 8 | reader->bytes[reader->at + k];
    reader->at += width;
    *argument = value;
    return TA_OK;
}

ta_status ta_cbor_decode(ta_cbor_document *document) {
    cbor_reader reader = {(const unsigned char *)document->bytes.pointer, document->bytes.length, 0};
    cbor_nesting nesting;
    nesting.depth = 0;
    uint32_t count = 0;
    document->count = 0;
    for (;;) {
        if (reader.at == reader.length) return TA_ERROR_INVALID; /* truncated */
        uint32_t initial = reader.bytes[reader.at++];
        uint32_t major = initial >> 5, info = initial & 0x1F;
        ta_cbor_token token = {0, 0, 0};
        uint64_t argument = 0;
        if (info == INFO_INDEFINITE)
            return major >= MAJOR_BYTES && major <= MAJOR_MAP ? TA_ERROR_UNSUPPORTED : TA_ERROR_INVALID;
        if (major == MAJOR_SIMPLE && info >= INFO_TWO_BYTES) {
            /* a float */
            ta_status status = read_argument(&reader, info, &argument);
            if (status != TA_OK) return status;
            token.kind = TA_CBOR_FLOAT;
            token.value = info == INFO_TWO_BYTES    ? half_widen((uint32_t)argument)
                          : info == INFO_FOUR_BYTES ? single_widen((uint32_t)argument)
                                                    : argument;
        } else {
            ta_status status = read_argument(&reader, info, &argument);
            if (status != TA_OK) return status;
            switch (major) {
            case MAJOR_UINT:
            case MAJOR_NEGINT:
                token.kind = major == MAJOR_UINT ? TA_CBOR_UINT : TA_CBOR_NEGINT;
                token.value = argument;
                break;
            case MAJOR_BYTES:
            case MAJOR_TEXT:
                if (argument > reader.length - reader.at) return TA_ERROR_INVALID;
                if (argument > UINT32_MAX) return TA_ERROR_UNSUPPORTED;
                if (major == MAJOR_TEXT && !utf8_valid(reader.bytes + reader.at, (size_t)argument))
                    return TA_ERROR_INVALID;
                token.kind = major == MAJOR_TEXT ? TA_CBOR_TEXT : TA_CBOR_BYTES;
                token.count = (uint32_t)argument;
                token.value = reader.at;
                reader.at += (size_t)argument;
                break;
            case MAJOR_ARRAY:
            case MAJOR_MAP: {
                /* every item takes at least one byte: a count past the rest is truncated */
                uint64_t items = major == MAJOR_MAP ? argument * 2 : argument;
                if (argument > UINT32_MAX || items > reader.length - reader.at) return TA_ERROR_INVALID;
                token.kind = major == MAJOR_MAP ? TA_CBOR_MAP : TA_CBOR_ARRAY;
                token.count = (uint32_t)argument;
                break;
            }
            case MAJOR_TAG:
                token.kind = TA_CBOR_TAG;
                token.value = argument;
                break;
            default: /* MAJOR_SIMPLE */
                /* a two-byte simple value below 32 is malformed (RFC 8949 §3.3) */
                if (info == INFO_ONE_BYTE && argument < 32) return TA_ERROR_INVALID;
                token.kind = TA_CBOR_SIMPLE;
                token.value = argument;
                break;
            }
        }
        if (count == UINT32_MAX) return TA_ERROR_UNSUPPORTED;
        if (count < document->capacity) document->tokens[count] = token;
        count++;

        uint64_t children = token.kind == TA_CBOR_TAG     ? 1
                            : token.kind == TA_CBOR_MAP   ? (uint64_t)token.count * 2
                            : token.kind == TA_CBOR_ARRAY ? token.count
                                                          : 0;
        if (children) {
            if (nesting_open(&nesting, children) != TA_OK) return TA_ERROR_INVALID;
        } else if (nesting_complete(&nesting)) {
            break;
        }
    }
    if (reader.at != reader.length) return TA_ERROR_INVALID; /* bytes after the item */
    document->count = count;
    return count > document->capacity ? TA_ERROR_NO_MEMORY : TA_OK;
}

ta_string_view ta_cbor_string(const ta_cbor_document *document, uint32_t at) {
    ta_string_view view = {"", 0};
    if (at >= document->count) return view;
    const ta_cbor_token *token = &document->tokens[at];
    if (token->kind != TA_CBOR_TEXT && token->kind != TA_CBOR_BYTES) return view;
    view.pointer = (const char *)document->bytes.pointer + token->value;
    view.length = token->count;
    return view;
}

uint32_t ta_cbor_next(const ta_cbor_document *document, uint32_t at) {
    if (at >= document->count) return TA_NIL;
    uint64_t pending = 1;
    while (pending) {
        if (at >= document->count) return TA_NIL;
        const ta_cbor_token *token = &document->tokens[at++];
        pending--;
        if (token->kind == TA_CBOR_ARRAY) pending += token->count;
        else if (token->kind == TA_CBOR_MAP) pending += (uint64_t)token->count * 2;
        else if (token->kind == TA_CBOR_TAG) pending++;
    }
    return at;
}

uint32_t ta_cbor_member(const ta_cbor_document *document, uint32_t map, ta_string_view key) {
    if (map >= document->count || document->tokens[map].kind != TA_CBOR_MAP) return TA_NIL;
    uint32_t pairs = document->tokens[map].count, at = map + 1;
    for (uint32_t pair = 0; pair < pairs; pair++) {
        uint32_t value = ta_cbor_next(document, at);
        if (value == TA_NIL) return TA_NIL;
        const ta_cbor_token *token = &document->tokens[at];
        if (token->kind == TA_CBOR_TEXT && token->count == key.length) {
            ta_string_view text = ta_cbor_string(document, at);
            if (ta_memcmp(text.pointer, key.pointer, key.length) == 0) return value;
        }
        at = ta_cbor_next(document, value);
        if (at == TA_NIL) return TA_NIL;
    }
    return TA_NIL;
}

/* ---- encoding ---------------------------------------------------------------- */

typedef struct cbor_output {
    unsigned char *bytes;
    size_t capacity, length; /* length goes on counting past capacity */
} cbor_output;

static void output_bytes(cbor_output *output, const void *bytes, size_t length) {
    if (output->length <= output->capacity && length <= output->capacity - output->length)
        ta_memcpy(output->bytes + output->length, bytes, length);
    output->length += length;
}

/* A head with `argument` in its shortest form. */
static void output_head(cbor_output *output, uint32_t major, uint64_t argument) {
    unsigned char head[9];
    size_t width;
    uint32_t info;
    if (argument < INFO_ONE_BYTE) width = 0, info = (uint32_t)argument;
    else if (argument <= 0xFF) width = 1, info = INFO_ONE_BYTE;
    else if (argument <= 0xFFFF) width = 2, info = INFO_TWO_BYTES;
    else if (argument <= 0xFFFFFFFF) width = 4, info = INFO_FOUR_BYTES;
    else width = 8, info = INFO_EIGHT_BYTES;
    head[0] = (unsigned char)(major << 5 | info);
    for (size_t k = 0; k < width; k++)
        head[1 + k] = (unsigned char)(argument >> (8 * (width - 1 - k)));
    output_bytes(output, head, 1 + width);
}

/* A float in the shortest width that holds it exactly. */
static void output_float(cbor_output *output, uint64_t bits) {
    uint32_t half;
    double value = bits_double(bits);
    if (value != value) { /* every NaN is the canonical one */
        static const unsigned char nan[3] = {0xF9, 0x7E, 0x00};
        output_bytes(output, nan, 3);
        return;
    }
    unsigned char head[9];
    size_t width;
    if (half_narrow(bits, &half)) {
        head[0] = 0xF9;
        width = 2;
        bits = half;
    } else if ((double)(float)value == value) {
        float single = (float)value;
        uint32_t single_bits;
        ta_memcpy(&single_bits, &single, sizeof single_bits);
        head[0] = 0xFA;
        width = 4;
        bits = single_bits;
    } else {
        head[0] = 0xFB;
        width = 8;
    }
    for (size_t k = 0; k < width; k++)
        head[1 + k] = (unsigned char)(bits >> (8 * (width - 1 - k)));
    output_bytes(output, head, 1 + width);
}

ta_status ta_cbor_encode(const ta_cbor_document *document, ta_buffer *out) {
    cbor_output output = {(unsigned char *)out->pointer, out->capacity, 0};
    cbor_nesting nesting;
    nesting.depth = 0;
    const unsigned char *bytes = (const unsigned char *)document->bytes.pointer;
    out->length = 0;
    uint32_t at = 0;
    for (;;) {
        if (at >= document->count) return TA_ERROR_INVALID; /* the item is not complete */
        const ta_cbor_token *token = &document->tokens[at++];
        uint64_t children = 0;
        switch (token->kind) {
        case TA_CBOR_UINT: output_head(&output, MAJOR_UINT, token->value); break;
        case TA_CBOR_NEGINT: output_head(&output, MAJOR_NEGINT, token->value); break;
        case TA_CBOR_BYTES:
        case TA_CBOR_TEXT:
            if (token->value > document->bytes.length || token->count > document->bytes.length - token->value)
                return TA_ERROR_INVALID;
            if (token->kind == TA_CBOR_TEXT && !utf8_valid(bytes + token->value, token->count))
                return TA_ERROR_INVALID;
            output_head(&output, token->kind == TA_CBOR_TEXT ? MAJOR_TEXT : MAJOR_BYTES, token->count);
            output_bytes(&output, bytes + token->value, token->count);
            break;
        case TA_CBOR_ARRAY:
            output_head(&output, MAJOR_ARRAY, token->count);
            children = token->count;
            break;
        case TA_CBOR_MAP:
            output_head(&output, MAJOR_MAP, token->count);
            children = (uint64_t)token->count * 2;
            break;
        case TA_CBOR_TAG:
            output_head(&output, MAJOR_TAG, token->value);
            children = 1;
            break;
        case TA_CBOR_SIMPLE:
            if (token->value > 0xFF || (token->value >= 24 && token->value < 32)) return TA_ERROR_INVALID;
            output_head(&output, MAJOR_SIMPLE, token->value);
            break;
        case TA_CBOR_FLOAT: output_float(&output, token->value); break;
        default: return TA_ERROR_INVALID;
        }
        if (children) {
            if (nesting_open(&nesting, children) != TA_OK) return TA_ERROR_INVALID;
        } else if (nesting_complete(&nesting)) {
            break;
        }
    }
    if (at != document->count) return TA_ERROR_INVALID; /* tokens after the item */
    out->length = output.length;
    return output.length > output.capacity ? TA_ERROR_NO_MEMORY : TA_OK;
}

/* ---- writing tokens from C ------------------------------------------------- */

static void write_token(ta_cbor_writer *writer, uint32_t kind, uint32_t count, uint64_t value) {
    ta_cbor_document *document = &writer->document;
    if (document->count == UINT32_MAX) {
        if (writer->status == TA_OK) writer->status = TA_ERROR_UNSUPPORTED;
        return;
    }
    if (document->count < document->capacity) {
        ta_cbor_token *token = &document->tokens[document->count];
        token->kind = kind;
        token->count = count;
        token->value = value;
    } else if (writer->status == TA_OK) {
        writer->status = TA_ERROR_NO_MEMORY;
    }
    document->count++;
}

static void write_string(ta_cbor_writer *writer, uint32_t kind, const void *pointer, size_t length) {
    if (length > UINT32_MAX) {
        if (writer->status == TA_OK) writer->status = TA_ERROR_UNSUPPORTED;
        return;
    }
    ta_buffer *strings = &writer->strings;
    size_t offset = strings->length;
    if (offset <= strings->capacity && length <= strings->capacity - offset) {
        if (length) ta_memcpy((char *)strings->pointer + offset, pointer, length);
    } else if (writer->status == TA_OK) {
        writer->status = TA_ERROR_NO_MEMORY;
    }
    strings->length += length;
    write_token(writer, kind, (uint32_t)length, offset);
}

void ta_cbor_write_map(ta_cbor_writer *writer, uint32_t pairs) { write_token(writer, TA_CBOR_MAP, pairs, 0); }

void ta_cbor_write_array(ta_cbor_writer *writer, uint32_t items) { write_token(writer, TA_CBOR_ARRAY, items, 0); }

void ta_cbor_write_text(ta_cbor_writer *writer, ta_string_view text) {
    write_string(writer, TA_CBOR_TEXT, text.pointer, text.length);
}

void ta_cbor_write_bytes(ta_cbor_writer *writer, ta_bytes bytes) {
    write_string(writer, TA_CBOR_BYTES, bytes.pointer, bytes.length);
}

void ta_cbor_write_uint(ta_cbor_writer *writer, uint64_t value) { write_token(writer, TA_CBOR_UINT, 0, value); }

void ta_cbor_write_int(ta_cbor_writer *writer, int64_t value) {
    if (value >= 0) write_token(writer, TA_CBOR_UINT, 0, (uint64_t)value);
    else write_token(writer, TA_CBOR_NEGINT, 0, (uint64_t)(-(value + 1)));
}

void ta_cbor_write_float(ta_cbor_writer *writer, double value) {
    write_token(writer, TA_CBOR_FLOAT, 0, double_bits(value));
}

void ta_cbor_write_simple(ta_cbor_writer *writer, uint32_t simple) {
    write_token(writer, TA_CBOR_SIMPLE, 0, simple);
}

void ta_cbor_write_tag(ta_cbor_writer *writer, uint64_t tag) { write_token(writer, TA_CBOR_TAG, 0, tag); }

ta_status ta_cbor_finish(ta_cbor_writer *writer, ta_buffer *out) {
    if (writer->status != TA_OK) {
        out->length = 0;
        return writer->status;
    }
    writer->document.bytes.pointer = writer->strings.pointer;
    writer->document.bytes.length = writer->strings.length;
    return ta_cbor_encode(&writer->document, out);
}
