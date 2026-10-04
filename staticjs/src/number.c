/*
 * Numbers without a C library: Number::toString (the shortest digits that
 * read back as the same double, Burger and Dybvig's free-format algorithm
 * over a small bignum), exact remainder and rounding by bit manipulation,
 * and Clinger's fast path for decimal literals.
 */
#include "internal.h"

typedef union {
    double d;
    uint64_t u;
} bits64;

static uint64_t to_bits(double x) {
    bits64 b;
    b.d = x;
    return b.u;
}

static double from_bits(uint64_t u) {
    bits64 b;
    b.u = u;
    return b.d;
}

bool sjs_number_is_nan(double x) {
    return x != x;
}

bool sjs_number_is_finite(double x) {
    return ((to_bits(x) >> 52) & 0x7FFu) != 0x7FFu;
}

bool sjs_number_is_negative_zero(double x) {
    return to_bits(x) == 0x8000000000000000ull;
}

double sjs_number_trunc(double x) {
    uint64_t u = to_bits(x);
    int32_t e = (int32_t)((u >> 52) & 0x7FFu) - 1023;
    if (e >= 52) return x;
    if (e < 0) return from_bits(u & 0x8000000000000000ull);
    uint64_t fraction = (1ull << (52 - e)) - 1u;
    return from_bits(u & ~fraction);
}

double sjs_number_floor(double x) {
    double t = sjs_number_trunc(x);
    return (x < 0 && t != x) ? t - 1.0 : t;
}

double sjs_number_ceil(double x) {
    double t = sjs_number_trunc(x);
    return (x > 0 && t != x) ? t + 1.0 : t;
}

double sjs_number_round(double x) {
    if (!sjs_number_is_finite(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    double f = sjs_number_floor(x);
    double rounded = (x - f >= 0.5) ? f + 1.0 : f;
    if (rounded == 0 && (x < 0 || sjs_number_is_negative_zero(x))) return -0.0;
    return rounded;
}

bool sjs_number_is_integer(double x) {
    return sjs_number_is_finite(x) && sjs_number_trunc(x) == x;
}

/*
 * x % y, exactly: the remainder of the magnitudes by binary long division,
 * with the sign of x.
 */
double sjs_number_remainder(double x, double y) {
    uint64_t ux = to_bits(x);
    uint64_t uy = to_bits(y);
    int32_t ex = (int32_t)((ux >> 52) & 0x7FFu);
    int32_t ey = (int32_t)((uy >> 52) & 0x7FFu);
    uint64_t sign = ux & 0x8000000000000000ull;
    if ((uy << 1) == 0 || sjs_number_is_nan(y) || ex == 0x7FF) return (x * y) / (x * y);
    if ((ux << 1) <= (uy << 1)) {
        if ((ux << 1) == (uy << 1)) return from_bits(sign);
        return x;
    }
    uint64_t mx;
    uint64_t my;
    if (ex == 0) {
        mx = ux << 12;
        ex = 0;
        while ((mx >> 63) == 0) {
            ex--;
            mx <<= 1;
        }
        mx = (ux & ((1ull << 52) - 1u)) << (1 - ex);
    } else {
        mx = (ux & ((1ull << 52) - 1u)) | (1ull << 52);
    }
    if (ey == 0) {
        my = uy << 12;
        ey = 0;
        while ((my >> 63) == 0) {
            ey--;
            my <<= 1;
        }
        my = (uy & ((1ull << 52) - 1u)) << (1 - ey);
    } else {
        my = (uy & ((1ull << 52) - 1u)) | (1ull << 52);
    }
    for (; ex > ey; ex--) {
        if (mx >= my) {
            mx -= my;
            if (mx == 0) return from_bits(sign);
        }
        mx <<= 1;
    }
    if (mx >= my) {
        mx -= my;
        if (mx == 0) return from_bits(sign);
    }
    while ((mx >> 52) == 0) {
        mx <<= 1;
        ex--;
    }
    if (ex > 0) {
        mx -= 1ull << 52;
        mx |= (uint64_t)ex << 52;
    } else {
        mx >>= (1 - ex);
    }
    return from_bits(mx | sign);
}

/* ------------------------------------------------------------------------
 * Bignums, just large enough for the scaled values of any double.
 */

#define BIG_WORDS 40u

typedef struct {
    uint32_t w[BIG_WORDS];
    uint32_t n;
} big;

static void big_set(big *b, uint64_t v) {
    b->n = 0;
    while (v != 0) {
        b->w[b->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static void big_shift_left(big *b, uint32_t bits) {
    uint32_t words = bits / 32u;
    uint32_t rest = bits % 32u;
    if (b->n == 0) return;
    uint32_t n = b->n + words + 1u;
    if (n > BIG_WORDS) n = BIG_WORDS;
    /* From the top down: each word reads only words below the ones written. */
    for (uint32_t i = n; i-- > 0;) {
        uint64_t current = (i >= words && i - words < b->n) ? b->w[i - words] : 0;
        uint64_t below = (i >= words + 1u && i - words - 1u < b->n) ? b->w[i - words - 1u] : 0;
        b->w[i] = rest == 0 ? (uint32_t)current : (uint32_t)((current << rest) | (below >> (32u - rest)));
    }
    b->n = n;
    while (b->n > 0 && b->w[b->n - 1] == 0) b->n--;
}

static void big_multiply_small(big *b, uint32_t m) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < b->n; i++) {
        uint64_t p = (uint64_t)b->w[i] * m + carry;
        b->w[i] = (uint32_t)p;
        carry = p >> 32;
    }
    if (carry != 0 && b->n < BIG_WORDS) b->w[b->n++] = (uint32_t)carry;
}

static void big_add(big *out, const big *a, const big *c) {
    uint32_t n = a->n > c->n ? a->n : c->n;
    uint64_t carry = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t s = carry;
        if (i < a->n) s += a->w[i];
        if (i < c->n) s += c->w[i];
        out->w[i] = (uint32_t)s;
        carry = s >> 32;
    }
    out->n = n;
    if (carry != 0 && n < BIG_WORDS) out->w[out->n++] = (uint32_t)carry;
}

static int big_compare(const big *a, const big *c) {
    if (a->n != c->n) return a->n < c->n ? -1 : 1;
    for (uint32_t i = a->n; i-- > 0;) {
        if (a->w[i] != c->w[i]) return a->w[i] < c->w[i] ? -1 : 1;
    }
    return 0;
}

/* a -= c, where a >= c. */
static void big_subtract(big *a, const big *c) {
    int64_t borrow = 0;
    for (uint32_t i = 0; i < a->n; i++) {
        int64_t d = (int64_t)a->w[i] - borrow - (int64_t)(i < c->n ? c->w[i] : 0);
        borrow = d < 0 ? 1 : 0;
        a->w[i] = (uint32_t)(d + (borrow << 32));
    }
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

static void big_multiply_pow10(big *b, int32_t power) {
    for (int32_t i = 0; i < power; i++) big_multiply_small(b, 10);
}

static uint32_t bit_length(uint64_t v) {
    uint32_t n = 0;
    while (v != 0) {
        n++;
        v >>= 1;
    }
    return n;
}

/*
 * The shortest decimal digits that read back as `value` (positive, finite,
 * not zero), the closest such when there are several, and the even one on
 * a tie: ECMA-262 Number::toString step 5. `digits` gets them as ASCII and
 * the result is their count; `*point` is n, with value = 0.d1d2... x 10^n.
 */
static uint32_t shortest_digits(double value, char *digits, int32_t *point) {
    uint64_t u = to_bits(value);
    uint32_t field = (uint32_t)((u >> 52) & 0x7FFu);
    uint64_t f = u & ((1ull << 52) - 1u);
    int32_t e;
    if (field == 0) {
        e = -1074;
    } else {
        f |= 1ull << 52;
        e = (int32_t)field - 1075;
    }
    bool even = (f & 1u) == 0;
    big r;
    big s;
    big plus;
    big minus;
    big sum;
    if (e >= 0) {
        if (f != (1ull << 52)) {
            big_set(&r, f);
            big_shift_left(&r, (uint32_t)e + 1u);
            big_set(&s, 2);
            big_set(&plus, 1);
            big_shift_left(&plus, (uint32_t)e);
            minus = plus;
        } else {
            big_set(&r, f);
            big_shift_left(&r, (uint32_t)e + 2u);
            big_set(&s, 4);
            big_set(&plus, 1);
            big_shift_left(&plus, (uint32_t)e + 1u);
            big_set(&minus, 1);
            big_shift_left(&minus, (uint32_t)e);
        }
    } else if (field <= 1 || f != (1ull << 52)) {
        big_set(&r, f);
        big_shift_left(&r, 1);
        big_set(&s, 1);
        big_shift_left(&s, (uint32_t)(1 - e));
        big_set(&plus, 1);
        big_set(&minus, 1);
    } else {
        big_set(&r, f);
        big_shift_left(&r, 2);
        big_set(&s, 1);
        big_shift_left(&s, (uint32_t)(2 - e));
        big_set(&plus, 2);
        big_set(&minus, 1);
    }
    /* An estimate of ceil(log10(value)), then corrected. */
    double estimate = (double)(e + (int32_t)bit_length(f) - 1) * 0.30102999566398114;
    int32_t k = (int32_t)sjs_number_ceil(estimate - 1e-10);
    if (k >= 0) {
        big_multiply_pow10(&s, k);
    } else {
        big_multiply_pow10(&r, -k);
        big_multiply_pow10(&plus, -k);
        big_multiply_pow10(&minus, -k);
    }
    for (;;) {
        big_add(&sum, &r, &plus);
        int c = big_compare(&sum, &s);
        if (even ? c >= 0 : c > 0) {
            big_multiply_small(&s, 10);
            k++;
            continue;
        }
        big_multiply_small(&sum, 10);
        c = big_compare(&sum, &s);
        if (even ? c < 0 : c <= 0) {
            big_multiply_small(&r, 10);
            big_multiply_small(&plus, 10);
            big_multiply_small(&minus, 10);
            k--;
            continue;
        }
        break;
    }
    *point = k;
    uint32_t count = 0;
    for (;;) {
        big_multiply_small(&r, 10);
        big_multiply_small(&plus, 10);
        big_multiply_small(&minus, 10);
        uint32_t d = 0;
        while (big_compare(&r, &s) >= 0) {
            big_subtract(&r, &s);
            d++;
        }
        int low = big_compare(&r, &minus);
        bool tc1 = even ? low <= 0 : low < 0;
        big_add(&sum, &r, &plus);
        int high = big_compare(&sum, &s);
        bool tc2 = even ? high >= 0 : high > 0;
        if (!tc1 && !tc2) {
            digits[count++] = (char)('0' + d);
            if (count >= 17) return count;
            continue;
        }
        if (tc1 && !tc2) {
            digits[count++] = (char)('0' + d);
        } else if (!tc1 && tc2) {
            digits[count++] = (char)('0' + d + 1);
        } else {
            big twice = r;
            big_multiply_small(&twice, 2);
            int c = big_compare(&twice, &s);
            uint32_t last = c < 0 ? d : (c > 0 ? d + 1 : ((d % 2u == 0) ? d : d + 1));
            digits[count++] = (char)('0' + last);
        }
        return count;
    }
}

static uint32_t put_int(char *out, uint32_t value) {
    char tmp[12];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

uint32_t sjs_number_format(double value, char *out) {
    uint32_t at = 0;
    if (sjs_number_is_nan(value)) {
        sjs_copy(out, "NaN", 3);
        return 3;
    }
    if (value == 0) {
        out[0] = '0';
        return 1;
    }
    if (value < 0) {
        out[at++] = '-';
        value = -value;
    }
    if (!sjs_number_is_finite(value)) {
        sjs_copy(out + at, "Infinity", 8);
        return at + 8;
    }
    char digits[20];
    int32_t n;
    int32_t k = (int32_t)shortest_digits(value, digits, &n);
    if (k <= n && n <= 21) {
        for (int32_t i = 0; i < k; i++) out[at++] = digits[i];
        for (int32_t i = k; i < n; i++) out[at++] = '0';
    } else if (0 < n && n <= 21) {
        for (int32_t i = 0; i < n; i++) out[at++] = digits[i];
        out[at++] = '.';
        for (int32_t i = n; i < k; i++) out[at++] = digits[i];
    } else if (-6 < n && n <= 0) {
        out[at++] = '0';
        out[at++] = '.';
        for (int32_t i = n; i < 0; i++) out[at++] = '0';
        for (int32_t i = 0; i < k; i++) out[at++] = digits[i];
    } else {
        out[at++] = digits[0];
        if (k > 1) {
            out[at++] = '.';
            for (int32_t i = 1; i < k; i++) out[at++] = digits[i];
        }
        out[at++] = 'e';
        int32_t exponent = n - 1;
        out[at++] = exponent < 0 ? '-' : '+';
        at += put_int(out + at, (uint32_t)(exponent < 0 ? -exponent : exponent));
    }
    return at;
}

static const double POWERS[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

bool sjs_number_from_decimal(sjs_str digits, int32_t exponent, double *out) {
    uint32_t start = 0;
    while (start < digits.length && digits.ptr[start] == '0') start++;
    uint32_t end = digits.length;
    while (end > start && digits.ptr[end - 1] == '0') {
        end--;
        exponent++;
    }
    if (start == end) {
        *out = 0;
        return true;
    }
    if (end - start > 16) return false;
    uint64_t mantissa = 0;
    for (uint32_t i = start; i < end; i++) mantissa = mantissa * 10u + (uint64_t)(digits.ptr[i] - '0');
    if (mantissa > (1ull << 53)) return false;
    double m = (double)mantissa;
    if (exponent == 0) {
        *out = m;
        return true;
    }
    if (exponent < 0) {
        if (exponent < -22) return false;
        *out = m / POWERS[-exponent];
        return true;
    }
    /* Move what does not fit in 10^22 into the mantissa while it stays exact. */
    while (exponent > 22) {
        if (mantissa > (1ull << 53) / 10u) return false;
        mantissa *= 10u;
        exponent--;
    }
    *out = (double)mantissa * POWERS[exponent];
    return true;
}
