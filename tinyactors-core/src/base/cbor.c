/* cbor.c — CBOR (RFC 8949) (include/tinyactors/cbor.h): token lists for
 * random access, a reader that yields one item at a time, a writer that
 * writes one item at a time into a room grown through a caller's allocator
 * (base/output.h), and diagnostic notation.
 *
 * Each walks the item tree with an explicit stack of how many items each
 * open container still has, so nothing recurses. */
#include <tinyactors/cbor.h>

#include "base/cbor_output.h"
#include "base/format.h"
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

/* The item at reader->at as a token (a string's value: its offset), moving
 * past its head and, for a string, its bytes. */
static ta_status read_item(cbor_reader *reader, ta_cbor_token *token) {
    if (reader->at == reader->length) return TA_ERROR_INVALID; /* truncated */
    uint32_t initial = reader->bytes[reader->at++];
    uint32_t major = initial >> 5, info = initial & 0x1F;
    uint64_t argument = 0;
    token->kind = 0;
    token->count = 0;
    token->value = 0;
    if (info == INFO_INDEFINITE)
        return major >= MAJOR_BYTES && major <= MAJOR_MAP ? TA_ERROR_UNSUPPORTED : TA_ERROR_INVALID;
    ta_status status = read_argument(reader, info, &argument);
    if (status != TA_OK) return status;
    if (major == MAJOR_SIMPLE && info >= INFO_TWO_BYTES) { /* a float */
        token->kind = TA_CBOR_FLOAT;
        token->value = info == INFO_TWO_BYTES    ? half_widen((uint32_t)argument)
                       : info == INFO_FOUR_BYTES ? single_widen((uint32_t)argument)
                                                 : argument;
        return TA_OK;
    }
    switch (major) {
    case MAJOR_UINT:
    case MAJOR_NEGINT:
        token->kind = major == MAJOR_UINT ? TA_CBOR_UINT : TA_CBOR_NEGINT;
        token->value = argument;
        break;
    case MAJOR_BYTES:
    case MAJOR_TEXT:
        if (argument > reader->length - reader->at) return TA_ERROR_INVALID;
        if (argument > UINT32_MAX) return TA_ERROR_UNSUPPORTED;
        if (major == MAJOR_TEXT && !utf8_valid(reader->bytes + reader->at, (size_t)argument))
            return TA_ERROR_INVALID;
        token->kind = major == MAJOR_TEXT ? TA_CBOR_TEXT : TA_CBOR_BYTES;
        token->count = (uint32_t)argument;
        token->value = reader->at;
        reader->at += (size_t)argument;
        break;
    case MAJOR_ARRAY:
    case MAJOR_MAP: {
        /* every item takes at least one byte: a count past the rest is truncated */
        uint64_t items = major == MAJOR_MAP ? argument * 2 : argument;
        if (argument > UINT32_MAX || items > reader->length - reader->at) return TA_ERROR_INVALID;
        token->kind = major == MAJOR_MAP ? TA_CBOR_MAP : TA_CBOR_ARRAY;
        token->count = (uint32_t)argument;
        break;
    }
    case MAJOR_TAG:
        token->kind = TA_CBOR_TAG;
        token->value = argument;
        break;
    default: /* MAJOR_SIMPLE */
        /* a two-byte simple value below 32 is malformed (RFC 8949 §3.3) */
        if (info == INFO_ONE_BYTE && argument < 32) return TA_ERROR_INVALID;
        token->kind = TA_CBOR_SIMPLE;
        token->value = argument;
        break;
    }
    return TA_OK;
}

/* The items a token's item holds: a tag's one, an array's, a map's keys and values. */
static uint64_t token_children(const ta_cbor_token *token) {
    return token->kind == TA_CBOR_TAG     ? 1
           : token->kind == TA_CBOR_MAP   ? (uint64_t)token->count * 2
           : token->kind == TA_CBOR_ARRAY ? token->count
                                          : 0;
}

ta_status ta_cbor_decode(ta_cbor_document *document) {
    cbor_reader reader = {(const unsigned char *)document->bytes.pointer, document->bytes.length, 0};
    cbor_nesting nesting;
    nesting.depth = 0;
    uint32_t count = 0;
    document->count = 0;
    for (;;) {
        ta_cbor_token token;
        ta_status status = read_item(&reader, &token);
        if (status != TA_OK) return status;
        if (count == UINT32_MAX) return TA_ERROR_UNSUPPORTED;
        if (count < document->capacity) document->tokens[count] = token;
        count++;
        uint64_t children = token_children(&token);
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

/* ---- reading, one item at a time ------------------------------------------- */

void ta_cbor_reader_init(ta_cbor_reader *reader, ta_bytes message) {
    reader->message = message;
    reader->at = 0;
    reader->depth = 0;
    reader->status = TA_OK;
}

int32_t ta_cbor_read(ta_cbor_reader *reader, ta_cbor_item *item) {
    if (reader->status != TA_OK) return reader->status;
    const unsigned char *bytes = (const unsigned char *)reader->message.pointer;
    if (reader->at && !reader->depth) { /* the item is read whole */
        if (reader->at == reader->message.length) return 0;
        reader->status = TA_ERROR_INVALID; /* bytes after the item */
        return reader->status;
    }
    cbor_reader cursor = {bytes, reader->message.length, reader->at};
    ta_cbor_token token;
    ta_status status = read_item(&cursor, &token);
    uint64_t children = token_children(&token);
    if (status == TA_OK && children && reader->depth == TA_CBOR_DEPTH_MAX) status = TA_ERROR_INVALID;
    if (status != TA_OK) {
        reader->status = status;
        return status;
    }
    item->kind = token.kind;
    item->count = token.count;
    item->value = token.value;
    item->bytes = (ta_bytes){(const void *)0, 0};
    item->depth = reader->depth;
    item->pad = 0;
    if (token.kind == TA_CBOR_TEXT || token.kind == TA_CBOR_BYTES) {
        item->bytes = (ta_bytes){bytes + token.value, token.count};
        item->value = 0;
    }
    reader->at = cursor.at;
    if (children) {
        reader->remaining[reader->depth++] = children;
    } else {
        while (reader->depth && !--reader->remaining[reader->depth - 1])
            reader->depth--;
    }
    return 1;
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

/* A head with `argument` in its shortest form. */
void cbor_output_head(ta_output *output, uint32_t major, uint64_t argument) {
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
    ta_output_write(output, head, 1 + width);
}

/* A float in the shortest width that holds it exactly. */
void cbor_output_float(ta_output *output, uint64_t bits) {
    uint32_t half;
    double value = bits_double(bits);
    if (value != value) { /* every NaN is the canonical one */
        static const unsigned char nan[3] = {0xF9, 0x7E, 0x00};
        ta_output_write(output, nan, 3);
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
    ta_output_write(output, head, 1 + width);
}

/* ---- diagnostic notation ----------------------------------------------------- */

/* Text that may be cut: what fits is written, and length goes on counting. */
static void output_text(ta_output *output, const char *text, size_t length) {
    if (output->length < output->capacity) {
        size_t room = output->capacity - output->length;
        ta_memcpy(output->bytes + output->length, text, length < room ? length : room);
    }
    output->length += length;
}

static void output_cstring(ta_output *output, const char *text) {
    size_t length = 0;
    while (text[length])
        length++;
    output_text(output, text, length);
}

static void output_decimal(ta_output *output, uint64_t value) {
    char digits[TA_FORMAT_U64_MAX];
    output_text(output, digits, ta_format_u64(digits, value));
}

static const char HEX[] = "0123456789abcdef";

/* A text string in quotes, escaped as JSON escapes it. */
static void output_quoted(ta_output *output, const unsigned char *text, size_t length) {
    output_text(output, "\"", 1);
    for (size_t k = 0; k < length; k++) {
        unsigned char c = text[k];
        const char *escape = c == '"'    ? "\\\""
                             : c == '\\' ? "\\\\"
                             : c == '\b' ? "\\b"
                             : c == '\f' ? "\\f"
                             : c == '\n' ? "\\n"
                             : c == '\r' ? "\\r"
                             : c == '\t' ? "\\t"
                                         : 0;
        if (escape) {
            output_cstring(output, escape);
        } else if (c < 0x20) {
            char unicode[6] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 15]};
            output_text(output, unicode, 6);
        } else {
            output_text(output, (const char *)&c, 1);
        }
    }
    output_text(output, "\"", 1);
}

/* ---- shortest decimals: Steele and White's free-format algorithm (Dragon4),
 * over integers of fixed size: the digits of a double that read back as it,
 * as few as there are. */

/* An unsigned integer of up to 44 32-bit words, least significant first:
 * room for 2^1076 × 10, the largest any step below makes. */
#define BIG_WORDS 44
typedef struct big {
    uint32_t word[BIG_WORDS];
    uint32_t length; /* words in use; the top one is not 0 */
} big;

static void big_from(big *b, uint64_t value) {
    b->word[0] = (uint32_t)value;
    b->word[1] = (uint32_t)(value >> 32);
    b->length = value >> 32 ? 2 : value ? 1 : 0;
}

static void big_multiply(big *b, uint32_t factor) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < b->length; i++) {
        uint64_t product = (uint64_t)b->word[i] * factor + carry;
        b->word[i] = (uint32_t)product;
        carry = product >> 32;
    }
    if (carry && b->length < BIG_WORDS) b->word[b->length++] = (uint32_t)carry;
}

static void big_shift(big *b, uint32_t bits) {
    for (; bits >= 16; bits -= 16)
        big_multiply(b, 1u << 16);
    if (bits) big_multiply(b, 1u << bits);
}

static int big_compare(const big *a, const big *b) {
    if (a->length != b->length) return a->length < b->length ? -1 : 1;
    for (uint32_t i = a->length; i-- > 0;)
        if (a->word[i] != b->word[i]) return a->word[i] < b->word[i] ? -1 : 1;
    return 0;
}

static void big_add(big *out, const big *a, const big *b) {
    const big *longer = a->length >= b->length ? a : b, *shorter = longer == a ? b : a;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < longer->length; i++) {
        uint64_t sum = (uint64_t)longer->word[i] + (i < shorter->length ? shorter->word[i] : 0) + carry;
        out->word[i] = (uint32_t)sum;
        carry = sum >> 32;
    }
    out->length = longer->length;
    if (carry && out->length < BIG_WORDS) out->word[out->length++] = (uint32_t)carry;
}

/* a -= b, for a >= b */
static void big_subtract(big *a, const big *b) {
    int64_t borrow = 0;
    for (uint32_t i = 0; i < a->length; i++) {
        int64_t difference = (int64_t)a->word[i] - (i < b->length ? b->word[i] : 0) - borrow;
        borrow = difference < 0;
        a->word[i] = (uint32_t)(difference + (borrow ? (int64_t)1 << 32 : 0));
    }
    while (a->length && !a->word[a->length - 1])
        a->length--;
}

/* The shortest digits that read back as the positive, finite double `bits`
 * (at most 17), with *point where the decimal point goes: the value is
 * 0.digits × 10^point. Returns how many digits. */
static uint32_t float_shortest(uint64_t bits, char *digits, int32_t *point) {
    uint64_t fraction = bits & UINT64_C(0xFFFFFFFFFFFFF);
    int32_t biased = (int32_t)((bits >> 52) & 0x7FF);
    uint64_t f = biased ? fraction | UINT64_C(1) << 52 : fraction;
    int32_t e = biased ? biased - 1075 : -1074;
    int even = !(f & 1);
    /* at a power of two the next double down is half as far as the next up */
    int lower_closer = !fraction && biased > 1;
    /* value = r / s; the doubles around it are half of m_minus, m_plus away */
    big r, s, m_plus, m_minus, t;
    if (e >= 0) {
        big_from(&r, f);
        big_shift(&r, (uint32_t)e + 1 + (uint32_t)lower_closer);
        big_from(&s, lower_closer ? 4 : 2);
        big_from(&m_minus, 1);
        big_shift(&m_minus, (uint32_t)e);
        big_from(&m_plus, 1);
        big_shift(&m_plus, (uint32_t)e + (uint32_t)lower_closer);
    } else {
        big_from(&r, f);
        big_shift(&r, 1 + (uint32_t)lower_closer);
        big_from(&s, 1);
        big_shift(&s, (uint32_t)(1 - e + lower_closer));
        big_from(&m_minus, 1);
        big_from(&m_plus, lower_closer ? 2 : 1);
    }
    /* an estimate of k = ceil(log10(value)), then corrected */
    uint32_t width = 0;
    for (uint64_t x = f; x; x >>= 1)
        width++;
    double estimate = (double)(e + (int32_t)width - 1) * 0.30102999566398114 + 1;
    int32_t k = (int32_t)estimate - (estimate < 0 && (double)(int32_t)estimate != estimate);
    for (int32_t i = 0; i < (k < 0 ? -k : k); i++) {
        if (k >= 0) {
            big_multiply(&s, 10);
        } else {
            big_multiply(&r, 10);
            big_multiply(&m_plus, 10);
            big_multiply(&m_minus, 10);
        }
    }
    for (;;) { /* too small a k: the value reaches 10^k */
        big_add(&t, &r, &m_plus);
        int high = big_compare(&t, &s);
        if (high < 0 || (high == 0 && !even)) break;
        big_multiply(&s, 10);
        k++;
    }
    for (;;) { /* too large a k: the value stays below 10^(k-1) */
        big_add(&t, &r, &m_plus);
        big_multiply(&t, 10);
        int high = big_compare(&t, &s);
        if (high > 0 || (high == 0 && even)) break;
        big_multiply(&r, 10);
        big_multiply(&m_plus, 10);
        big_multiply(&m_minus, 10);
        k--;
    }
    *point = k;
    uint32_t count = 0;
    for (;;) {
        big_multiply(&r, 10);
        big_multiply(&m_plus, 10);
        big_multiply(&m_minus, 10);
        uint32_t digit = 0;
        while (big_compare(&r, &s) >= 0) {
            big_subtract(&r, &s);
            digit++;
        }
        int low_compare = big_compare(&r, &m_minus);
        big_add(&t, &r, &m_plus);
        int high_compare = big_compare(&t, &s);
        int low = low_compare < 0 || (even && low_compare == 0);
        int high = high_compare > 0 || (even && high_compare == 0);
        if (low && high) { /* both read back: the nearer one */
            big_add(&t, &r, &r);
            if (big_compare(&t, &s) >= 0) digit++;
        } else if (high) {
            digit++;
        }
        digits[count++] = (char)('0' + digit);
        if (low || high || count == 17) break;
    }
    return count;
}

/* A float in diagnostic notation: the shortest decimal that reads back as
 * it, with a point or an exponent (1.0, 0.1, 1.0e+300); NaN, Infinity. */
static void output_float_text(ta_output *output, uint64_t bits) {
    uint32_t biased = (uint32_t)((bits >> 52) & 0x7FF);
    uint64_t fraction = bits & UINT64_C(0xFFFFFFFFFFFFF);
    if (biased == 0x7FF && fraction) {
        output_cstring(output, "NaN");
        return;
    }
    if (bits >> 63) output_text(output, "-", 1);
    if (biased == 0x7FF) {
        output_cstring(output, "Infinity");
        return;
    }
    if (!biased && !fraction) {
        output_cstring(output, "0.0");
        return;
    }
    char digits[17];
    int32_t point;
    int32_t length = (int32_t)float_shortest(bits & ~(UINT64_C(1) << 63), digits, &point);
    if (point > 21 || point < -5) { /* d.ddde±x */
        output_text(output, digits, 1);
        output_text(output, ".", 1);
        if (length > 1) output_text(output, digits + 1, (size_t)(length - 1));
        else output_text(output, "0", 1);
        output_text(output, point - 1 < 0 ? "e-" : "e+", 2);
        output_decimal(output, (uint64_t)(point - 1 < 0 ? 1 - point : point - 1));
    } else if (point >= length) { /* an integer: ddd000.0 */
        output_text(output, digits, (size_t)length);
        for (int32_t k = length; k < point; k++)
            output_text(output, "0", 1);
        output_text(output, ".0", 2);
    } else if (point > 0) { /* dd.ddd */
        output_text(output, digits, (size_t)point);
        output_text(output, ".", 1);
        output_text(output, digits + point, (size_t)(length - point));
    } else { /* 0.000ddd */
        output_text(output, "0.", 2);
        for (int32_t k = 0; k < -point; k++)
            output_text(output, "0", 1);
        output_text(output, digits, (size_t)length);
    }
}

ta_status ta_cbor_diagnostic(ta_bytes cbor, ta_buffer *out) {
    cbor_reader reader = {(const unsigned char *)cbor.pointer, cbor.length, 0};
    ta_output output = ta_output_over(*out, NULL);
    /* per open container: what closes it, and the items it has seen */
    char closer[TA_CBOR_DEPTH_MAX];
    uint64_t seen[TA_CBOR_DEPTH_MAX];
    cbor_nesting nesting;
    nesting.depth = 0;
    out->length = 0;
    for (;;) {
        if (nesting.depth) { /* the separator before this item */
            uint32_t top = nesting.depth - 1;
            if (closer[top] == ']' && seen[top]) output_text(&output, ",", 1);
            if (closer[top] == '}' && seen[top]) output_text(&output, seen[top] & 1 ? ":" : ",", 1);
            seen[top]++;
        }
        if (reader.at == reader.length) return TA_ERROR_INVALID; /* truncated */
        uint32_t initial = reader.bytes[reader.at++];
        uint32_t major = initial >> 5, info = initial & 0x1F;
        uint64_t argument = 0, children = 0;
        if (info == INFO_INDEFINITE)
            return major >= MAJOR_BYTES && major <= MAJOR_MAP ? TA_ERROR_UNSUPPORTED : TA_ERROR_INVALID;
        ta_status status = read_argument(&reader, info, &argument);
        if (status != TA_OK) return status;
        char open = 0;
        switch (major) {
        case MAJOR_UINT: output_decimal(&output, argument); break;
        case MAJOR_NEGINT:
            if (argument == UINT64_MAX) output_cstring(&output, "-18446744073709551616");
            else output_text(&output, "-", 1), output_decimal(&output, argument + 1);
            break;
        case MAJOR_BYTES:
        case MAJOR_TEXT:
            if (argument > reader.length - reader.at) return TA_ERROR_INVALID;
            if (major == MAJOR_TEXT) {
                if (!utf8_valid(reader.bytes + reader.at, (size_t)argument)) return TA_ERROR_INVALID;
                output_quoted(&output, reader.bytes + reader.at, (size_t)argument);
            } else {
                output_text(&output, "h'", 2);
                for (uint64_t k = 0; k < argument; k++) {
                    char pair[2] = {HEX[reader.bytes[reader.at + k] >> 4], HEX[reader.bytes[reader.at + k] & 15]};
                    output_text(&output, pair, 2);
                }
                output_text(&output, "'", 1);
            }
            reader.at += (size_t)argument;
            break;
        case MAJOR_ARRAY:
        case MAJOR_MAP:
            children = major == MAJOR_MAP ? argument * 2 : argument;
            if (argument > UINT32_MAX || children > reader.length - reader.at) return TA_ERROR_INVALID;
            output_text(&output, major == MAJOR_MAP ? "{" : "[", 1);
            open = major == MAJOR_MAP ? '}' : ']';
            if (!children) output_text(&output, &open, 1);
            break;
        case MAJOR_TAG:
            output_decimal(&output, argument);
            output_text(&output, "(", 1);
            children = 1;
            open = ')';
            break;
        default: /* MAJOR_SIMPLE */
            if (info >= INFO_TWO_BYTES) {
                output_float_text(&output, info == INFO_TWO_BYTES    ? half_widen((uint32_t)argument)
                                           : info == INFO_FOUR_BYTES ? single_widen((uint32_t)argument)
                                                                     : argument);
            } else if (info == INFO_ONE_BYTE && argument < 32) {
                return TA_ERROR_INVALID;
            } else {
                static const char *const NAMES[4] = {"false", "true", "null", "undefined"};
                if (argument >= TA_CBOR_FALSE && argument <= TA_CBOR_UNDEFINED) {
                    output_cstring(&output, NAMES[argument - TA_CBOR_FALSE]);
                } else {
                    output_cstring(&output, "simple(");
                    output_decimal(&output, argument);
                    output_text(&output, ")", 1);
                }
            }
            break;
        }
        if (children) {
            if (nesting_open(&nesting, children) != TA_OK) return TA_ERROR_INVALID;
            closer[nesting.depth - 1] = open;
            seen[nesting.depth - 1] = 0;
            continue;
        }
        /* the item is complete: close every container it completes */
        int done = 1;
        while (nesting.depth) {
            if (--nesting.remaining[nesting.depth - 1]) {
                done = 0;
                break;
            }
            output_text(&output, &closer[--nesting.depth], 1);
        }
        if (done) break;
    }
    if (reader.at != reader.length) return TA_ERROR_INVALID; /* bytes after the item */
    out->length = output.length;
    return output.length > output.capacity ? TA_ERROR_NO_MEMORY : TA_OK;
}

/* ---- writing, one item at a time ------------------------------------------- */

#define WRITER_LATER 0x80u /* a container's count is fixed when it closes */

static void writer_fail(ta_cbor_writer *writer, ta_status status) {
    if (writer->status == TA_OK || (writer->status == TA_ERROR_NO_MEMORY && status != TA_ERROR_NO_MEMORY))
        writer->status = status;
}

/* Writing, or only counting what the item needs once the room was full:
 * the item's structure is followed either way. */
static int writer_going(const ta_cbor_writer *writer) {
    return writer->status == TA_OK || writer->status == TA_ERROR_NO_MEMORY;
}

/* The writer's memory as an output (base/output.h): grow it, write, and keep
 * where it now is. */
static ta_output writer_output(const ta_cbor_writer *writer) {
    ta_output output;
    output.bytes = writer->bytes;
    output.capacity = writer->capacity;
    output.length = writer->length;
    output.allocator = writer->allocator;
    output.room = writer->room;
    return output;
}

static void writer_keep(ta_cbor_writer *writer, const ta_output *output) {
    writer->bytes = output->bytes;
    writer->capacity = output->capacity;
    writer->length = output->length;
    if (!ta_output_fits(output)) writer_fail(writer, TA_ERROR_NO_MEMORY);
}

/* An item is whole: counted in the container it is in, closing those it
 * completes; the top-level item is the writer's one. */
static void writer_complete(ta_cbor_writer *writer) {
    while (writer->depth) {
        uint32_t top = writer->depth - 1;
        if (writer->kinds[top] & WRITER_LATER) {
            writer->counts[top]++;
            return;
        }
        if (--writer->counts[top]) return;
        writer->depth--;
    }
    if (++writer->items > 1) writer_fail(writer, TA_ERROR_INVALID); /* one item per writer */
}

/* A head (or a float) and, for a string, its bytes. */
static void writer_put(ta_cbor_writer *writer, const unsigned char *head, size_t head_length,
                       ta_bytes bytes) {
    if (!writer_going(writer)) return;
    if (writer->depth == 0 && writer->items) {
        writer_fail(writer, TA_ERROR_INVALID); /* one item per writer */
        return;
    }
    if (writer->status == TA_ERROR_NO_MEMORY) { /* counted: what the item needs */
        writer->length += head_length + bytes.length;
        return;
    }
    ta_output output = writer_output(writer);
    if (ta_output_reserve(&output, head_length + bytes.length)) {
        ta_output_write(&output, head, head_length);
        ta_output_write(&output, bytes.pointer, bytes.length);
    } else {
        output.length += head_length + bytes.length;
    }
    writer_keep(writer, &output);
}

/* The head of `major` with `argument`, shortest; its length. */
static size_t head_of(unsigned char *head, uint32_t major, uint64_t argument) {
    ta_output output = ta_output_over((ta_buffer){head, 9, 0}, (const ta_allocator *)0);
    cbor_output_head(&output, major, argument);
    return output.length;
}

static void writer_scalar(ta_cbor_writer *writer, uint32_t major, uint64_t argument) {
    unsigned char head[9];
    writer_put(writer, head, head_of(head, major, argument), (ta_bytes){(const void *)0, 0});
    if (writer_going(writer)) writer_complete(writer);
}

/* A container or tag opens: `count` items follow (TA_CBOR_COUNT_LATER: as
 * many as are written before ta_cbor_write_end). */
static void writer_open(ta_cbor_writer *writer, uint32_t kind, uint64_t count, int later) {
    if (!writer_going(writer)) return;
    uint32_t major = kind == TA_CBOR_MAP ? MAJOR_MAP : kind == TA_CBOR_ARRAY ? MAJOR_ARRAY : MAJOR_TAG;
    if (!later && kind != TA_CBOR_TAG && count == 0) { /* nothing follows: whole now */
        writer_scalar(writer, major, 0);
        return;
    }
    if (writer->depth == TA_CBOR_DEPTH_MAX) {
        writer_fail(writer, TA_ERROR_INVALID);
        return;
    }
    size_t at = writer->length;
    unsigned char head[9];
    /* counted later: one byte now, the shortest head once the count is known */
    size_t length = later ? (head[0] = (unsigned char)(major << 5), 1) : head_of(head, major, count);
    writer_put(writer, head, length, (ta_bytes){(const void *)0, 0});
    if (!writer_going(writer)) return;
    writer->heads[writer->depth] = at;
    writer->counts[writer->depth] = later ? 0 : (kind == TA_CBOR_MAP ? count * 2 : count);
    writer->kinds[writer->depth] = (uint8_t)(kind | (later ? WRITER_LATER : 0u));
    writer->depth++;
}

void ta_cbor_writer_init(ta_cbor_writer *writer, ta_buffer room, const ta_allocator *allocator) {
    writer->bytes = (unsigned char *)room.pointer;
    writer->capacity = room.pointer ? room.capacity : 0;
    writer->length = 0;
    writer->allocator = allocator;
    writer->room = writer->bytes;
    writer->status = TA_OK;
    writer->depth = 0;
    writer->items = 0;
    writer->pad = 0;
}

void ta_cbor_write_map(ta_cbor_writer *writer, uint32_t pairs) {
    writer_open(writer, TA_CBOR_MAP, pairs, pairs == TA_CBOR_COUNT_LATER);
}

void ta_cbor_write_array(ta_cbor_writer *writer, uint32_t items) {
    writer_open(writer, TA_CBOR_ARRAY, items, items == TA_CBOR_COUNT_LATER);
}

void ta_cbor_write_tag(ta_cbor_writer *writer, uint64_t tag) {
    if (!writer_going(writer)) return;
    if (writer->depth == TA_CBOR_DEPTH_MAX) {
        writer_fail(writer, TA_ERROR_INVALID);
        return;
    }
    unsigned char head[9];
    writer_put(writer, head, head_of(head, MAJOR_TAG, tag), (ta_bytes){(const void *)0, 0});
    if (!writer_going(writer)) return;
    writer->heads[writer->depth] = 0;
    writer->counts[writer->depth] = 1;
    writer->kinds[writer->depth] = (uint8_t)TA_CBOR_TAG;
    writer->depth++;
}

void ta_cbor_write_end(ta_cbor_writer *writer) {
    if (!writer_going(writer)) return;
    uint32_t top = writer->depth ? writer->depth - 1 : 0;
    if (!writer->depth || !(writer->kinds[top] & WRITER_LATER)) {
        writer_fail(writer, TA_ERROR_INVALID); /* nothing counted later is open */
        return;
    }
    uint32_t kind = writer->kinds[top] & ~WRITER_LATER;
    uint64_t count = writer->counts[top];
    if (kind == TA_CBOR_MAP && (count & 1)) {
        writer_fail(writer, TA_ERROR_INVALID); /* a key without its value */
        return;
    }
    if (kind == TA_CBOR_MAP) count /= 2;
    unsigned char head[9];
    size_t length = head_of(head, kind == TA_CBOR_MAP ? MAJOR_MAP : MAJOR_ARRAY, count);
    size_t at = writer->heads[top];
    ta_output output = writer_output(writer);
    if (writer->status != TA_OK) {
        output.length += length - 1; /* counted: the longer head */
    } else if (length == 1 || ta_output_reserve(&output, length - 1)) {
        /* the items move up to make room for a longer head */
        if (length > 1)
            ta_memmove(output.bytes + at + length, output.bytes + at + 1, output.length - at - 1);
        output.length += length - 1;
        ta_memcpy(output.bytes + at, head, length);
    } else {
        output.length += length - 1;
    }
    writer_keep(writer, &output);
    writer->depth--;
    writer_complete(writer);
}

static void writer_string(ta_cbor_writer *writer, uint32_t major, ta_bytes bytes) {
    if (bytes.length > UINT32_MAX) {
        writer_fail(writer, TA_ERROR_UNSUPPORTED);
        return;
    }
    if (major == MAJOR_TEXT && !utf8_valid((const unsigned char *)bytes.pointer, bytes.length)) {
        writer_fail(writer, TA_ERROR_INVALID);
        return;
    }
    unsigned char head[9];
    writer_put(writer, head, head_of(head, major, bytes.length), bytes);
    if (writer_going(writer)) writer_complete(writer);
}

void ta_cbor_write_text(ta_cbor_writer *writer, ta_string_view text) {
    writer_string(writer, MAJOR_TEXT, (ta_bytes){text.pointer, text.length});
}

void ta_cbor_write_bytes(ta_cbor_writer *writer, ta_bytes bytes) {
    writer_string(writer, MAJOR_BYTES, bytes);
}

void ta_cbor_write_uint(ta_cbor_writer *writer, uint64_t value) {
    writer_scalar(writer, MAJOR_UINT, value);
}

void ta_cbor_write_int(ta_cbor_writer *writer, int64_t value) {
    if (value >= 0) writer_scalar(writer, MAJOR_UINT, (uint64_t)value);
    else writer_scalar(writer, MAJOR_NEGINT, (uint64_t)(-(value + 1)));
}

void ta_cbor_write_negative(ta_cbor_writer *writer, uint64_t n) {
    writer_scalar(writer, MAJOR_NEGINT, n);
}

void ta_cbor_write_float(ta_cbor_writer *writer, double value) {
    unsigned char head[9];
    ta_output output = ta_output_over((ta_buffer){head, 9, 0}, (const ta_allocator *)0);
    cbor_output_float(&output, double_bits(value));
    writer_put(writer, head, output.length, (ta_bytes){(const void *)0, 0});
    if (writer_going(writer)) writer_complete(writer);
}

void ta_cbor_write_simple(ta_cbor_writer *writer, uint32_t simple) {
    if (simple > 0xFF || (simple >= 24 && simple < 32)) {
        writer_fail(writer, TA_ERROR_INVALID);
        return;
    }
    writer_scalar(writer, MAJOR_SIMPLE, simple);
}

ta_status ta_cbor_writer_bytes(const ta_cbor_writer *writer, ta_bytes *out) {
    *out = (ta_bytes){(const void *)0, 0};
    if (writer->status != TA_OK) return writer->status;
    if (writer->depth || writer->items != 1) return TA_ERROR_INVALID; /* not one whole item */
    *out = (ta_bytes){writer->bytes, writer->length};
    return TA_OK;
}

void ta_cbor_writer_free(ta_cbor_writer *writer) {
    ta_output output = writer_output(writer);
    ta_output_free(&output);
    writer->bytes = writer->room;
    writer->capacity = 0;
    writer->length = 0;
}
