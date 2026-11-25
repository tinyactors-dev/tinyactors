/* format.c — decimal numbers and bounded text (base/format.h). */
#include "base/format.h"

#include "base/memory.h"

uint32_t ta_format_u64(char *buffer, uint64_t value) {
    char reversed[TA_FORMAT_U64_MAX];
    uint32_t n = 0;
    do {
        reversed[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value);
    for (uint32_t i = 0; i < n; i++)
        buffer[i] = reversed[n - 1 - i];
    return n;
}

void ta_text_append(ta_text *text, ta_string_view view) {
    if (text->overflow || view.length > text->capacity - text->length) {
        text->overflow = 1;
        return;
    }
    if (view.length) ta_memcpy(text->pointer + text->length, view.pointer, view.length);
    text->length += view.length;
}

void ta_text_append_u64(ta_text *text, uint64_t value) {
    char digits[TA_FORMAT_U64_MAX];
    uint32_t n = ta_format_u64(digits, value);
    ta_text_append(text, (ta_string_view){digits, n});
}

/* ---- JSON's strings and shortest decimals, into an output ------------------ */

/* Text that may be cut: the output grows when it can, and when it cannot,
 * what fits is written and length goes on counting (ta_cbor_diagnostic's
 * contract: a room too small holds the text's first bytes). */
static void format_text(ta_output *output, const char *text, size_t length) {
    if (length && ta_output_reserve(output, length)) {
        ta_memcpy(output->bytes + output->length, text, length);
    } else if (output->length < output->capacity) {
        size_t room = output->capacity - output->length;
        ta_memcpy(output->bytes + output->length, text, length < room ? length : room);
    }
    output->length += length;
}

static void format_cstring(ta_output *output, const char *text) {
    size_t length = 0;
    while (text[length])
        length++;
    format_text(output, text, length);
}

static void format_decimal(ta_output *output, uint64_t value) {
    char digits[TA_FORMAT_U64_MAX];
    format_text(output, digits, ta_format_u64(digits, value));
}

static const char format_hex_digits[] = "0123456789abcdef";

void ta_format_quoted(ta_output *output, ta_string_view text) {
    format_text(output, "\"", 1);
    for (uint32_t k = 0; k < text.length; k++) {
        unsigned char c = (unsigned char)text.pointer[k];
        const char *escape = c == '"'    ? "\\\""
                             : c == '\\' ? "\\\\"
                             : c == '\b' ? "\\b"
                             : c == '\f' ? "\\f"
                             : c == '\n' ? "\\n"
                             : c == '\r' ? "\\r"
                             : c == '\t' ? "\\t"
                                         : 0;
        if (escape) {
            format_cstring(output, escape);
        } else if (c < 0x20) {
            char unicode[6] = {'\\', 'u', '0', '0', format_hex_digits[c >> 4], format_hex_digits[c & 15]};
            format_text(output, unicode, 6);
        } else {
            format_text(output, (const char *)&c, 1);
        }
    }
    format_text(output, "\"", 1);
}

/* ---- shortest decimals: Steele and White's free-format algorithm (Dragon4),
 * over integers of fixed size: the digits of a double that read back as it,
 * as few as there are. */

/* An unsigned integer of up to 44 32-bit words, least significant first:
 * room for 2^1076 × 10, the largest any step below makes. */
enum { BIG_WORDS = 44 };
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

void ta_format_double(ta_output *output, double value) {
    uint64_t bits;
    ta_memcpy(&bits, &value, sizeof bits);
    uint32_t biased = (uint32_t)((bits >> 52) & 0x7FF);
    uint64_t fraction = bits & UINT64_C(0xFFFFFFFFFFFFF);
    if (biased == 0x7FF && fraction) {
        format_cstring(output, "NaN");
        return;
    }
    if (bits >> 63) format_text(output, "-", 1);
    if (biased == 0x7FF) {
        format_cstring(output, "Infinity");
        return;
    }
    if (!biased && !fraction) {
        format_cstring(output, "0.0");
        return;
    }
    char digits[17];
    int32_t point;
    int32_t length = (int32_t)float_shortest(bits & ~(UINT64_C(1) << 63), digits, &point);
    if (point > 21 || point < -5) { /* d.ddde±x */
        format_text(output, digits, 1);
        format_text(output, ".", 1);
        if (length > 1) format_text(output, digits + 1, (size_t)(length - 1));
        else format_text(output, "0", 1);
        format_text(output, point - 1 < 0 ? "e-" : "e+", 2);
        format_decimal(output, (uint64_t)(point - 1 < 0 ? 1 - point : point - 1));
    } else if (point >= length) { /* an integer: ddd000.0 */
        format_text(output, digits, (size_t)length);
        for (int32_t k = length; k < point; k++)
            format_text(output, "0", 1);
        format_text(output, ".0", 2);
    } else if (point > 0) { /* dd.ddd */
        format_text(output, digits, (size_t)point);
        format_text(output, ".", 1);
        format_text(output, digits + point, (size_t)(length - point));
    } else { /* 0.000ddd */
        format_text(output, "0.", 2);
        for (int32_t k = 0; k < -point; k++)
            format_text(output, "0", 1);
        format_text(output, digits, (size_t)length);
    }
}
