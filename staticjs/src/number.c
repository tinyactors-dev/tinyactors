/*
 * Numbers without a C library: Number::toString (the shortest digits that
 * read back as the same double, Burger and Dybvig's free-format algorithm
 * over a small bignum), exact remainder and rounding by bit manipulation,
 * and reading numbers, correctly rounded for any number of digits
 * (Clinger's fast path, then the bignum).
 */
#include "internal.h"

/* Every operation below rounds once, as JavaScript's do: never a fused
 * multiply-add, which a compiler may otherwise make of a * b + c. */
#pragma STDC FP_CONTRACT OFF

bool sjs_number_is_nan(double x) {
    return x != x;
}

bool sjs_number_is_finite(double x) {
    return ((sjs_double_bits(x) >> 52) & 0x7FFu) != 0x7FFu;
}

bool sjs_number_is_negative_zero(double x) {
    return sjs_double_bits(x) == 0x8000000000000000ull;
}

double sjs_number_trunc(double x) {
    uint64_t u = sjs_double_bits(x);
    int32_t e = (int32_t)((u >> 52) & 0x7FFu) - 1023;
    if (e >= 52) return x;
    if (e < 0) return sjs_bits_double(u & 0x8000000000000000ull);
    uint64_t fraction = (1ull << (52 - e)) - 1u;
    return sjs_bits_double(u & ~fraction);
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
    uint64_t ux = sjs_double_bits(x);
    uint64_t uy = sjs_double_bits(y);
    int32_t ex = (int32_t)((ux >> 52) & 0x7FFu);
    int32_t ey = (int32_t)((uy >> 52) & 0x7FFu);
    uint64_t sign = ux & 0x8000000000000000ull;
    if ((uy << 1) == 0 || sjs_number_is_nan(y) || ex == 0x7FF) return (x * y) / (x * y);
    if ((ux << 1) <= (uy << 1)) {
        if ((ux << 1) == (uy << 1)) return sjs_bits_double(sign);
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
            if (mx == 0) return sjs_bits_double(sign);
        }
        mx <<= 1;
    }
    if (mx >= my) {
        mx -= my;
        if (mx == 0) return sjs_bits_double(sign);
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
    return sjs_bits_double(mx | sign);
}

/* ------------------------------------------------------------------------
 * Bignums, just large enough for the scaled values of any double, and for
 * reading a decimal: at most 781 significant digits (2,595 bits) over
 * 10^1104 (3,668 bits) scaled by 2^64, doubled (big_quotient), fits in
 * 3,733 bits.
 */

#define BIG_WORDS 128u

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

static void big_copy(big *to, const big *from) {
    for (uint32_t i = 0; i < from->n; i++) to->w[i] = from->w[i];
    to->n = from->n;
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
    uint64_t u = sjs_double_bits(value);
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
            big_copy(&minus, &plus);
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
            big twice;
            big_copy(&twice, &r);
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
    /* An integer below 2^32 is its own shortest digits, written without
     * the big-number arithmetic: a key or an index (`String(0)`, a pluck
     * by a small number in Object.groupBy) costs some 30 instructions
     * instead of 450. */
    if (value < 4294967296.0 && value == (double)(uint32_t)value) return at + put_int(out + at, (uint32_t)value);
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

/* ------------------------------------------------------------------------
 * toFixed, toExponential and toPrecision: the decimal value of a double is
 * exact (it has at most 767 significant digits), and each method rounds it
 * half up ("if there are two such n, pick the larger n"), which is whether
 * the first digit dropped is 5 or more.
 */

/* A positive value as r / s x 10^point, with r / s in [0.1, 1): its digits
 * are taken one at a time, exactly. */
typedef struct {
    big r;
    big s;
    int32_t point;
} exact_digits;

static void exact_start(double value, exact_digits *x) {
    uint64_t u = sjs_double_bits(value);
    uint32_t field = (uint32_t)((u >> 52) & 0x7FFu);
    uint64_t f = u & ((1ull << 52) - 1u);
    int32_t e;
    if (field == 0) {
        e = -1074;
    } else {
        f |= 1ull << 52;
        e = (int32_t)field - 1075;
    }
    big_set(&x->r, f);
    big_set(&x->s, 1);
    if (e >= 0) big_shift_left(&x->r, (uint32_t)e);
    else big_shift_left(&x->s, (uint32_t)-e);
    /* An estimate of log10(value) from below, then corrected both ways. */
    double estimate = (double)(e + (int32_t)bit_length(f) - 1) * 0.30102999566398114;
    int32_t k = (int32_t)sjs_number_ceil(estimate - 1e-10);
    if (k >= 0) big_multiply_pow10(&x->s, k);
    else big_multiply_pow10(&x->r, -k);
    while (big_compare(&x->r, &x->s) >= 0) {
        big_multiply_small(&x->s, 10);
        k++;
    }
    for (;;) {
        big tenfold;
        big_copy(&tenfold, &x->r);
        big_multiply_small(&tenfold, 10);
        if (big_compare(&tenfold, &x->s) >= 0) break;
        big_copy(&x->r, &tenfold);
        k--;
    }
    x->point = k;
}

/* The next digit. */
static uint32_t exact_next(exact_digits *x) {
    big_multiply_small(&x->r, 10);
    uint32_t d = 0;
    while (big_compare(&x->r, &x->s) >= 0) {
        big_subtract(&x->r, &x->s);
        d++;
    }
    return d;
}

/* The next `count` digits (at least 1) into `digits`, rounded half up on
 * the digit after them. A carry out of the first digit leaves 1 and zeros,
 * and moves the point. */
static void exact_take(exact_digits *x, uint32_t count, char *digits) {
    for (uint32_t i = 0; i < count; i++) digits[i] = (char)('0' + exact_next(x));
    if (exact_next(x) < 5u) return;
    for (uint32_t i = count; i-- > 0;) {
        if (digits[i] != '9') {
            digits[i]++;
            return;
        }
        digits[i] = '0';
    }
    digits[0] = '1';
    x->point++;
}

static uint32_t put_zeros(char *out, int32_t count) {
    for (int32_t i = 0; i < count; i++) out[i] = '0';
    return count > 0 ? (uint32_t)count : 0;
}

/* d.ddd...e±x: `count` digits, the exponent `exponent`. */
typedef struct {
    const char *digits;
    uint32_t count;
    int32_t exponent;
} exponential_form;

static uint32_t put_exponential(char *out, exponential_form form) {
    const char *digits = form.digits;
    uint32_t count = form.count;
    int32_t exponent = form.exponent;
    uint32_t at = 0;
    out[at++] = digits[0];
    if (count > 1) {
        out[at++] = '.';
        for (uint32_t i = 1; i < count; i++) out[at++] = digits[i];
    }
    out[at++] = 'e';
    out[at++] = exponent < 0 ? '-' : '+';
    return at + put_int(out + at, (uint32_t)(exponent < 0 ? -exponent : exponent));
}

/* toFixed of a value in (0, 1e21) or 0: n, the integer nearest
 * value x 10^f, written with f digits after the point. */
static uint32_t put_fixed(char *out, double value, int32_t f) {
    char digits[SJS_NUMBER_DIGITS_MAX];
    uint32_t count = 1;
    digits[0] = '0';
    if (value != 0) {
        exact_digits x;
        exact_start(value, &x);
        int32_t point = x.point;
        int32_t kept = point + f; /* the digits down to 10^-f */
        if (kept >= 1) {
            exact_take(&x, (uint32_t)kept, digits);
            count = (uint32_t)kept;
            if (x.point != point) digits[count++] = '0';
        } else if (kept == 0 && exact_next(&x) >= 5u) {
            digits[0] = '1';
        }
    }
    if (f == 0) {
        sjs_copy(out, digits, count);
        return count;
    }
    uint32_t at = 0;
    int32_t whole = (int32_t)count - f;
    if (whole <= 0) {
        out[at++] = '0';
        out[at++] = '.';
        at += put_zeros(out + at, -whole);
        sjs_copy(out + at, digits, count);
        return at + count;
    }
    sjs_copy(out, digits, (uint32_t)whole);
    at = (uint32_t)whole;
    out[at++] = '.';
    sjs_copy(out + at, digits + whole, (uint32_t)f);
    return at + (uint32_t)f;
}

/* toPrecision of a positive value or 0, p digits. */
static uint32_t put_precision(char *out, double value, int32_t p) {
    char digits[SJS_NUMBER_DIGITS_MAX];
    int32_t e = 0;
    if (value == 0) {
        put_zeros(digits, p);
    } else {
        exact_digits x;
        exact_start(value, &x);
        exact_take(&x, (uint32_t)p, digits);
        e = x.point - 1;
    }
    if (e < -6 || e >= p) return put_exponential(out, (exponential_form){ digits, (uint32_t)p, e });
    uint32_t at = 0;
    if (e < 0) {
        out[at++] = '0';
        out[at++] = '.';
        at += put_zeros(out + at, -(e + 1));
        sjs_copy(out + at, digits, (uint32_t)p);
        return at + (uint32_t)p;
    }
    sjs_copy(out, digits, (uint32_t)e + 1u);
    at = (uint32_t)e + 1u;
    if (e == p - 1) return at;
    out[at++] = '.';
    sjs_copy(out + at, digits + e + 1, (uint32_t)(p - e - 1));
    return at + (uint32_t)(p - e - 1);
}

/* toExponential of a positive value or 0: f digits after the point, or as
 * few as read back as the value (Number::toString's) when f is -1. */
static uint32_t put_exponent_form(char *out, double value, int32_t f) {
    char digits[SJS_NUMBER_DIGITS_MAX];
    if (value == 0) {
        uint32_t count = f < 0 ? 1u : (uint32_t)f + 1u;
        put_zeros(digits, (int32_t)count);
        return put_exponential(out, (exponential_form){ digits, count, 0 });
    }
    if (f < 0) {
        int32_t point;
        uint32_t count = shortest_digits(value, digits, &point);
        return put_exponential(out, (exponential_form){ digits, count, point - 1 });
    }
    exact_digits x;
    exact_start(value, &x);
    exact_take(&x, (uint32_t)f + 1u, digits);
    return put_exponential(out, (exponential_form){ digits, (uint32_t)f + 1u, x.point - 1 });
}

uint32_t sjs_number_format_digits(sjs_number_digits d, char *out) {
    double value = d.value;
    if (!sjs_number_is_finite(value)) return sjs_number_format(value, out);
    if (d.mode == SJS_DIGITS_PRECISION && d.digits < 0) return sjs_number_format(value, out);
    if (d.mode == SJS_DIGITS_FIXED && (value >= 1e21 || value <= -1e21)) return sjs_number_format(value, out);
    uint32_t at = 0;
    if (value < 0) {
        out[at++] = '-';
        value = -value;
    }
    value += 0.0; /* -0 is 0 */
    if (d.mode == SJS_DIGITS_FIXED) return at + put_fixed(out + at, value, d.digits);
    if (d.mode == SJS_DIGITS_PRECISION) return at + put_precision(out + at, value, d.digits);
    return at + put_exponent_form(out + at, value, d.digits);
}

/* ------------------------------------------------------------------------
 * Number.prototype.toString(radix). Radix 10 is String(x). Any other radix
 * is written as V8 writes it, which is neither the shortest text that
 * reads back nor the exact value: DoubleToRadixCString in V8's
 * src/numbers/conversions.cc, as of V8 12.4 (Node 22), translated to C
 * operation for operation, every one an IEEE double operation rounded once
 * (FP_CONTRACT is off above). The integer part is written exactly, with
 * zeros for the digits under the double's precision; the fraction digit by
 * digit, multiplying by the radix, until what is left is under `delta`,
 * half the distance to the next double (multiplied by the radix too), and
 * rounded to even at the last digit, carrying into the digits before it
 * and the integer part. The library calls it makes (floor, fmod, the
 * next double and the binary exponent) are the exact bit operations of
 * this file. Its notice:
 *
 * Copyright 2006-2011, the V8 project authors. All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Google Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

static const char RADIX_DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";

/* The digit a character of RADIX_DIGITS stands for. */
static int32_t radix_digit_value(char c) {
    return c > '9' ? c - 'a' + 10 : c - '0';
}

/* A text being written in a radix: the fraction from the middle of
 * `buffer` on, then the integer part before it. */
typedef struct {
    char *buffer; /* SJS_NUMBER_RADIX_MAX bytes */
    uint32_t radix;
    double integer; /* the integer part, plus one if the fraction carried into it */
} radix_text;

/* The fraction of `value` (positive, finite), from the middle of the
 * buffer, where the point goes, on; sets t->integer. Returns the cursor
 * after the last byte written. */
static uint32_t radix_fraction(radix_text *t, double value) {
    char *buffer = t->buffer;
    uint32_t radix = t->radix;
    double *integer = &t->integer;
    uint32_t point = SJS_NUMBER_RADIX_MAX / 2u;
    uint32_t cursor = point;
    *integer = sjs_number_floor(value);
    double fraction = value - *integer;
    /* Half the distance to the next double (base::Double::NextDouble), at
     * least the smallest double: 0.5 times the smallest rounds to 0. */
    double delta = 0.5 * (sjs_bits_double(sjs_double_bits(value) + 1u) - value);
    double smallest = sjs_bits_double(1u);
    if (!(delta > smallest)) delta = smallest;
    if (!(fraction >= delta)) return cursor;
    buffer[cursor++] = '.';
    do {
        fraction *= (double)radix;
        delta *= (double)radix;
        int32_t digit = (int32_t)fraction;
        buffer[cursor++] = RADIX_DIGITS[digit];
        fraction -= (double)digit;
        if (fraction > 0.5 || (fraction == 0.5 && (digit & 1) != 0)) {
            if (fraction + delta > 1) {
                /* Round up: carry back through the digits written. */
                for (;;) {
                    cursor--;
                    if (cursor == point) {
                        *integer += 1;
                        break;
                    }
                    digit = radix_digit_value(buffer[cursor]);
                    if ((uint32_t)digit + 1u < radix) {
                        buffer[cursor++] = RADIX_DIGITS[digit + 1];
                        break;
                    }
                }
                break;
            }
        }
    } while (fraction >= delta);
    return cursor;
}

uint32_t sjs_number_format_radix(double value, uint32_t radix, char *out) {
    if (radix == 10u || !sjs_number_is_finite(value) || value == 0) return sjs_number_format(value, out);
    bool negative = value < 0;
    if (negative) value = -value;
    radix_text t = { out, radix, 0 };
    uint32_t end = radix_fraction(&t, value);
    double integer = t.integer;
    uint32_t start = SJS_NUMBER_RADIX_MAX / 2u;
    /* Zeros for the digits under the double's precision, while integer /
     * radix is at least 2^53 (base::Double::Exponent() > 0: a biased
     * exponent over 1075). */
    while (((sjs_double_bits(integer / (double)radix) >> 52) & 0x7FFu) > 1075u) {
        integer /= (double)radix;
        out[--start] = '0';
    }
    do {
        double remainder = sjs_number_remainder(integer, (double)radix); /* fmod, exact */
        out[--start] = RADIX_DIGITS[(int32_t)remainder];
        integer = (integer - remainder) / (double)radix;
    } while (integer > 0);
    if (negative) out[--start] = '-';
    uint32_t length = end - start;
    for (uint32_t i = 0; i < length; i++) out[i] = out[start + i];
    return length;
}

/* ------------------------------------------------------------------------
 * Reading numbers: digits to the nearest double, ties to even, for any
 * number of digits, as JavaScript reads a literal and as Number,
 * parseFloat and parseInt read a string (RoundMVResult; V8 rounds every one
 * correctly). Clinger's fast path where one rounding is exact; past it, the
 * bignums above.
 */

static const double POWERS[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* Rounding a decimal correctly needs at most 768 significant digits (the
 * longest midpoint between two doubles has 767). Past them only whether a
 * digit is not 0 matters, which a 1 after the first DECIMAL_DIGITS_MAX
 * stands for: no midpoint lies between the digits kept and that 1. */
#define DECIMAL_DIGITS_MAX 780u

/* q x 2^e2 with q's top bit set; `sticky` when the value read is a little
 * more than that. */
typedef struct {
    uint64_t q;
    int32_t e2;
    bool sticky;
} binary_value;

/* The double nearest a binary_value, ties to even: a normal double keeps
 * 53 of q's bits, a subnormal fewer. */
static double round_binary(binary_value v) {
    int32_t top = v.e2 + 63; /* the exponent of q's top bit */
    if (top > 1023) return 1.0 / 0.0;
    int32_t drop = top >= -1022 ? 11 : -1011 - top;
    if (drop > 64) return 0.0;
    uint64_t kept = drop == 64 ? 0 : v.q >> drop;
    uint64_t rest = drop == 64 ? v.q : v.q & ((1ull << drop) - 1u);
    uint64_t half = 1ull << (drop - 1);
    if (rest > half || (rest == half && (v.sticky || (kept & 1u) != 0))) kept++;
    if (drop > 11) return sjs_bits_double(kept); /* subnormal: 2^52 is the smallest normal */
    if (kept == (1ull << 53)) {
        kept >>= 1;
        top++;
        if (top > 1023) return 1.0 / 0.0;
    }
    return sjs_bits_double(((uint64_t)(top + 1023) << 52) | (kept & ((1ull << 52) - 1u)));
}

static uint32_t big_bits(const big *b) {
    return b->n == 0 ? 0 : (b->n - 1u) * 32u + bit_length(b->w[b->n - 1u]);
}

static void big_add_small(big *b, uint32_t v) {
    uint64_t carry = v;
    for (uint32_t i = 0; carry != 0; i++) {
        if (i == b->n) {
            b->w[b->n++] = (uint32_t)carry;
            return;
        }
        uint64_t s = (uint64_t)b->w[i] + carry;
        b->w[i] = (uint32_t)s;
        carry = s >> 32;
    }
}

static void big_scale10(big *b, uint32_t power) {
    static const uint32_t TENS[9] = { 1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u };
    for (; power >= 9u; power -= 9u) big_multiply_small(b, 1000000000u);
    if (power != 0) big_multiply_small(b, TENS[power]);
}

/* A positive integer's top 64 bits. */
static binary_value big_top(const big *b) {
    uint32_t bits = big_bits(b);
    binary_value v = { 0, (int32_t)bits - 64, false };
    for (uint32_t i = 0; i < 64u; i++) {
        v.q <<= 1;
        if (i < bits) {
            uint32_t at = bits - 1u - i;
            v.q |= (b->w[at / 32u] >> (at % 32u)) & 1u;
        }
    }
    if (bits > 64u) {
        uint32_t below = bits - 64u;
        for (uint32_t i = 0; i < below / 32u; i++) v.sticky = v.sticky || b->w[i] != 0;
        if (below % 32u != 0 && (b->w[below / 32u] & ((1u << (below % 32u)) - 1u)) != 0) v.sticky = true;
    }
    return v;
}

/* n / s (both positive), to 64 bits by long division: scaled so that the
 * quotient lies in (2^62, 2^64), then a bit at a time until the top bit is
 * set. Uses up n and s. */
static binary_value big_quotient(big *n, big *s) {
    int32_t k = 63 - (int32_t)big_bits(n) + (int32_t)big_bits(s);
    if (k > 0) big_shift_left(n, (uint32_t)k);
    big_shift_left(s, 64u + (uint32_t)(k < 0 ? -k : 0));
    binary_value v = { 0, 0, false };
    int32_t steps = 0;
    while ((v.q >> 63) == 0) {
        big_shift_left(n, 1);
        v.q <<= 1;
        if (big_compare(n, s) >= 0) {
            big_subtract(n, s);
            v.q |= 1u;
        }
        steps++;
    }
    v.e2 = 64 - steps - k;
    v.sticky = n->n != 0;
    return v;
}

/* The significant digits of a decimal: `count` digits from `first`, a '.'
 * among them skipped, times 10^exponent. */
typedef struct {
    sjs_str text;
    uint32_t first;
    uint32_t count;
    int64_t exponent;
} decimal;

/* The next `count` digits from text[*at], skipping a '.', as an integer. */
static uint64_t decimal_chunk(const decimal *d, uint32_t *at, uint32_t count) {
    uint64_t v = 0;
    while (count > 0) {
        char c = d->text.ptr[(*at)++];
        if (c == '.') continue;
        v = v * 10u + (uint64_t)(c - '0');
        count--;
    }
    return v;
}

/* Clinger: digits worth at most 2^53 and a power of ten up to 10^22 are
 * exact doubles, and one division or multiplication rounds them once. */
static bool decimal_fast(const decimal *d, double *out) {
    if (d->count > 16u || d->exponent < -22 || d->exponent > 22 + 16) return false;
    uint32_t at = d->first;
    uint64_t mantissa = decimal_chunk(d, &at, d->count);
    if (mantissa > (1ull << 53)) return false;
    int32_t exponent = (int32_t)d->exponent;
    if (exponent <= 0) {
        *out = exponent == 0 ? (double)mantissa : (double)mantissa / POWERS[-exponent];
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

/* big_quotient's long division for n < 2^64 over s = 10^p with p <= 18
 * (s < 2^63), in two words: the remainder stays under s x 2^64, and twice
 * it under 2^128. What a 17-digit decimal such as 0.30000000000000004
 * needs, without the bignums. */
static binary_value small_quotient(uint64_t n, uint64_t s) {
    int32_t k = 63 - (int32_t)bit_length(n) + (int32_t)bit_length(s); /* at least 3 */
    uint64_t high = k >= 64 ? n << (k - 64) : n >> (64 - k);
    uint64_t low = k >= 64 ? 0 : n << k;
    binary_value v = { 0, 0, false };
    int32_t steps = 0;
    while ((v.q >> 63) == 0) {
        high = (high << 1) | (low >> 63);
        low <<= 1;
        v.q <<= 1;
        if (high >= s) {
            high -= s;
            v.q |= 1u;
        }
        steps++;
    }
    v.e2 = 64 - steps - k;
    v.sticky = (high | low) != 0;
    return v;
}

static double decimal_slow(const decimal *d) {
    if (d->count <= 19u && d->exponent < 0 && d->exponent >= -18) {
        uint32_t at = d->first;
        uint64_t n = decimal_chunk(d, &at, d->count);
        uint64_t s = 1;
        for (int64_t i = d->exponent; i < 0; i++) s *= 10u;
        return round_binary(small_quotient(n, s));
    }
    big n;
    big_set(&n, 0);
    uint32_t used = d->count > DECIMAL_DIGITS_MAX ? DECIMAL_DIGITS_MAX : d->count;
    int64_t exponent = d->exponent + (int64_t)(d->count - used);
    uint32_t at = d->first;
    for (uint32_t done = 0; done < used;) {
        uint32_t take = used - done < 9u ? used - done : 9u;
        big_scale10(&n, take);
        big_add_small(&n, (uint32_t)decimal_chunk(d, &at, take));
        done += take;
    }
    if (used < d->count) {
        big_multiply_small(&n, 10);
        big_add_small(&n, 1);
        exponent--;
    }
    if (exponent >= 0) {
        big_scale10(&n, (uint32_t)exponent);
        return round_binary(big_top(&n));
    }
    big s;
    big_set(&s, 1);
    big_scale10(&s, (uint32_t)-exponent);
    return round_binary(big_quotient(&n, &s));
}

double sjs_number_from_decimal(sjs_str digits, int64_t exponent) {
    decimal d = { digits, 0, 0, exponent };
    uint32_t seen = 0;
    uint32_t first = 0;
    uint32_t last = 0;
    bool any = false;
    for (uint32_t i = 0; i < digits.length; i++) {
        char c = digits.ptr[i];
        if (c == '.') continue;
        if (c != '0') {
            if (!any) {
                d.first = i;
                first = seen;
            }
            any = true;
            last = seen;
        }
        seen++;
    }
    if (!any) return 0;
    d.count = last - first + 1u;
    d.exponent += (int64_t)(seen - 1u - last);
    /* The value lies in [10^(top - 1), 10^top). */
    int64_t top = (int64_t)d.count + d.exponent;
    if (top > 309) return 1.0 / 0.0;
    if (top <= -324) return 0;
    double out;
    if (decimal_fast(&d, &out)) return out;
    return decimal_slow(&d);
}

/* A digit's value in any radix up to 36; 36 for anything else. */
static uint32_t radix_digit(char c) {
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'z') return (uint32_t)(c - 'a') + 10u;
    if (c >= 'A' && c <= 'Z') return (uint32_t)(c - 'A') + 10u;
    return 36;
}

double sjs_number_from_binary(sjs_str digits, uint32_t radix) {
    uint32_t bits = radix == 2u ? 1u : radix == 4u ? 2u : radix == 8u ? 3u : radix == 16u ? 4u : 5u;
    binary_value v = { 0, 0, false };
    for (uint32_t i = 0; i < digits.length; i++) {
        uint32_t d = radix_digit(digits.ptr[i]);
        if ((v.q >> (64u - bits)) == 0) {
            v.q = (v.q << bits) | d;
        } else {
            /* Past 60 bits, the rest only scales and marks: 2^4096 is
             * Infinity already. */
            if (v.e2 < 4096) v.e2 += (int32_t)bits;
            if (d != 0) v.sticky = true;
        }
    }
    if (v.q == 0) return 0;
    while ((v.q >> 63) == 0) {
        v.q <<= 1;
        v.e2--;
    }
    return round_binary(v);
}

/* The longest StrUnsignedDecimalLiteral at s[at] but Infinity: digits, a
 * point and digits (one side may be empty, not both), and an exponent when
 * it has digits. `end` stays `at` when there is none. */
typedef struct {
    uint32_t end;
    sjs_str digits; /* the mantissa, with its point */
    int64_t exponent;
} decimal_literal;

static bool is_decimal_digit(char c) {
    return c >= '0' && c <= '9';
}

static decimal_literal read_decimal(sjs_str s, uint32_t at) {
    decimal_literal r = { at, { s.ptr + at, 0 }, 0 };
    uint32_t i = at;
    while (i < s.length && is_decimal_digit(s.ptr[i])) i++;
    bool any = i > at;
    if (i < s.length && s.ptr[i] == '.') {
        uint32_t j = i + 1u;
        while (j < s.length && is_decimal_digit(s.ptr[j])) j++;
        if (any || j > i + 1u) {
            r.exponent = -(int64_t)(j - i - 1u);
            i = j;
            any = true;
        }
    }
    if (!any) return r;
    r.digits.length = i - at;
    r.end = i;
    if (i < s.length && (s.ptr[i] == 'e' || s.ptr[i] == 'E')) {
        uint32_t j = i + 1u;
        int64_t sign = 1;
        if (j < s.length && (s.ptr[j] == '+' || s.ptr[j] == '-')) {
            if (s.ptr[j] == '-') sign = -1;
            j++;
        }
        if (j < s.length && is_decimal_digit(s.ptr[j])) {
            /* Saturated: 10^(10^11) is Infinity, whatever the digits. */
            int64_t e = 0;
            for (; j < s.length && is_decimal_digit(s.ptr[j]); j++) {
                if (e < 100000000000) e = e * 10 + (s.ptr[j] - '0');
            }
            r.exponent += sign * e;
            r.end = j;
        }
    }
    return r;
}

/* Where StrWhiteSpaceChar ends from s[at] on. */
static uint32_t skip_white_space(sjs_str s, uint32_t at) {
    while (at < s.length) {
        uint32_t next = at;
        if (!sjs_is_white_space((uint32_t)sjs_utf8_next(s, &next))) break;
        at = next;
    }
    return at;
}

/* A sign at s[*at], taken: whether it is '-'. */
static bool read_sign(sjs_str s, uint32_t *at) {
    if (*at < s.length && (s.ptr[*at] == '+' || s.ptr[*at] == '-')) return s.ptr[(*at)++] == '-';
    return false;
}

static bool starts_with_infinity(sjs_str s, uint32_t at) {
    return s.length - at >= 8u && sjs_str_equal((sjs_str){ s.ptr + at, 8 }, SJS_STR("Infinity"));
}

double sjs_number_parse(sjs_str s) {
    uint32_t start = skip_white_space(s, 0);
    uint32_t end = start;
    for (uint32_t i = start; i < s.length;) {
        if (!sjs_is_white_space((uint32_t)sjs_utf8_next(s, &i))) end = i;
    }
    sjs_str body = { s.ptr + start, end - start };
    if (body.length == 0) return 0;
    if (body.length > 2u && body.ptr[0] == '0') {
        char p = body.ptr[1];
        uint32_t radix = (p == 'x' || p == 'X') ? 16u : (p == 'o' || p == 'O') ? 8u : (p == 'b' || p == 'B') ? 2u : 0u;
        if (radix != 0) {
            sjs_str digits = { body.ptr + 2, body.length - 2u };
            for (uint32_t i = 0; i < digits.length; i++) {
                if (radix_digit(digits.ptr[i]) >= radix) return 0.0 / 0.0;
            }
            return sjs_number_from_binary(digits, radix);
        }
    }
    uint32_t at = 0;
    bool negative = read_sign(body, &at);
    double v;
    if (starts_with_infinity(body, at) && body.length - at == 8u) {
        v = 1.0 / 0.0;
    } else {
        decimal_literal d = read_decimal(body, at);
        if (d.end == at || d.end != body.length) return 0.0 / 0.0;
        v = sjs_number_from_decimal(d.digits, d.exponent);
    }
    return negative ? -v : v;
}

double sjs_number_parse_float(sjs_str s) {
    uint32_t at = skip_white_space(s, 0);
    bool negative = read_sign(s, &at);
    double v;
    if (starts_with_infinity(s, at)) {
        v = 1.0 / 0.0;
    } else {
        decimal_literal d = read_decimal(s, at);
        if (d.end == at) return 0.0 / 0.0;
        v = sjs_number_from_decimal(d.digits, d.exponent);
    }
    return negative ? -v : v;
}

/* parseInt's value for a radix that is not 10 or a power of two, which the
 * specification lets an implementation approximate past 20 digits: V8's
 * (src/numbers/conversions.cc), so that it is Node's. Digits are read into
 * parts while a part's multiplier stays under 2^32 / 36, and each part is
 * added as number * multiplier + part.
 *
 * How that expression rounds depends on the machine V8 runs on: clang
 * contracts it into one fused multiply-add where the baseline instruction set
 * has one (aarch64), and rounds the product and the sum apart where it does
 * not (x86_64). staticjs rounds as Node does on the same machine; elsewhere
 * (wasm) as on x86_64. */
#if defined(__aarch64__)
#define RADIX_FUSED 1
#else
#define RADIX_FUSED 0
#endif
static double radix_accumulate(sjs_str digits, uint32_t radix) {
    double number = 0;
    uint32_t i = 0;
    bool done = false;
    while (!done) {
        uint32_t part = 0;
        uint32_t multiplier = 1;
        for (;;) {
            if (i == digits.length) {
                done = true;
                break;
            }
            uint32_t m = multiplier * radix;
            if (m > 0xFFFFFFFFu / 36u) break;
            part = part * radix + radix_digit(digits.ptr[i]);
            multiplier = m;
            i++;
        }
#if RADIX_FUSED
        number = __builtin_fma(number, (double)multiplier, (double)part); /* fmadd */
#else
        double scaled = number * (double)multiplier;
        number = scaled + (double)part;
#endif
    }
    return number;
}

double sjs_number_parse_int(sjs_str s, int32_t radix) {
    uint32_t at = skip_white_space(s, 0);
    bool negative = read_sign(s, &at);
    bool strip = true;
    if (radix != 0) {
        if (radix < 2 || radix > 36) return 0.0 / 0.0;
        strip = radix == 16;
    } else {
        radix = 10;
    }
    if (strip && s.length - at >= 2u && s.ptr[at] == '0' && (s.ptr[at + 1u] == 'x' || s.ptr[at + 1u] == 'X')) {
        at += 2u;
        radix = 16;
    }
    uint32_t end = at;
    while (end < s.length && radix_digit(s.ptr[end]) < (uint32_t)radix) end++;
    if (end == at) return 0.0 / 0.0;
    while (at < end && s.ptr[at] == '0') at++;
    sjs_str digits = { s.ptr + at, end - at };
    double v;
    if (digits.length == 0) v = 0;
    else if (radix == 10) v = sjs_number_from_decimal(digits, 0);
    else if ((radix & (radix - 1)) == 0) v = sjs_number_from_binary(digits, (uint32_t)radix);
    else v = radix_accumulate(digits, (uint32_t)radix);
    return negative ? -v : v;
}

/* ToUint32: the integer toward zero, modulo 2^32; NaN and the infinities
 * give 0. ToInt32 is the same bits, signed. */
uint32_t sjs_number_to_uint32(double x) {
    uint64_t u = sjs_double_bits(x);
    uint32_t field = (uint32_t)((u >> 52) & 0x7FFu);
    /* NaN and the infinities; zero, subnormals and anything else under 1. */
    if (field == 0x7FFu || field < 1023u) return 0;
    int32_t e = (int32_t)field - 1075; /* |x| = m x 2^e */
    uint64_t m = (u & ((1ull << 52) - 1u)) | (1ull << 52);
    uint32_t low;
    if (e >= 32) low = 0;
    else if (e >= 0) low = (uint32_t)(m << e);
    else low = (uint32_t)(m >> -e);
    return (u >> 63) != 0 ? 0u - low : low;
}

double sjs_number_sqrt(double x) {
    /* An instruction on every target (sqrtsd, fsqrt, f64.sqrt), correctly
     * rounded by IEEE 754; unlike __builtin_sqrt, never a call to libm's
     * sqrt for errno. */
    return __builtin_elementwise_sqrt(x);
}

double sjs_number_fround(double x) {
    return (double)(float)x;
}

/* Math.hypot as V8 computes it (MathHypot, src/builtins/math.tq): every
 * argument seen first, for the largest magnitude and whether one is NaN;
 * then an Infinity gives Infinity, a NaN NaN, and a largest of 0 (or no
 * arguments) 0; otherwise each magnitude divided by the largest, squared
 * and summed with Kahan's compensation, and sqrt(sum) * max. */
static double hypot_magnitude(double x) {
    return x < 0 ? -x : x + 0.0;
}

void sjs_hypot_see(sjs_hypot *h, double x) {
    if (sjs_number_is_nan(x)) h->nan = true;
    else if (hypot_magnitude(x) > h->max) h->max = hypot_magnitude(x);
}

bool sjs_hypot_settled(const sjs_hypot *h, double *out) {
    if (!sjs_number_is_finite(h->max)) *out = h->max;
    else if (h->nan) *out = 0.0 / 0.0;
    else if (h->max == 0) *out = 0;
    else return false;
    return true;
}

void sjs_hypot_add(sjs_hypot *h, double x) {
    double n = hypot_magnitude(x) / h->max;
    double square = n * n;
    double summand = square - h->compensation;
    double preliminary = h->sum + summand;
    h->compensation = (preliminary - h->sum) - summand;
    h->sum = preliminary;
}

double sjs_hypot_end(const sjs_hypot *h) {
    return sjs_number_sqrt(h->sum) * h->max;
}

/* Number of a BigInt: its top 64 bits, rounded as a decimal's are. */
double sjs_number_from_top(sjs_number_top v) {
    return round_binary((binary_value){ v.q, v.e2, v.sticky });
}

/* ------------------------------------------------------------------------
 * Math.random: V8's generator, so that a seed gives the values
 * `node --random-seed=<seed>` gives. Following V8 12.4 (Node 22):
 * src/numbers/math-random.cc, MathRandom::RefillCache, and
 * src/base/utils/random-number-generator.{h,cc}, MurmurHash3, XorShift128
 * and ToDouble. Their notice:
 *
 * Copyright 2013 (random-number-generator) and 2018 (math-random), the V8
 * project authors. All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Google Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * V8 seeds the state as s0 = MurmurHash3(seed), s1 = MurmurHash3(~seed)
 * (the seed an int64, sign-extended from the flag's int), fills a cache of
 * 64 doubles at once, each the next xorshift128+ step's s0 as the bits of
 * a double in [1, 2) minus 1, and Math.random takes them from the last to
 * the first. Here no cache is kept: the cursor starts at the state after
 * the refill, which gives the cache's last value, and steps back one
 * xorshift128+ step per draw (the step is invertible), so a draw costs a
 * step either way and the evaluator holds four words, not 64 doubles.
 */
#define RANDOM_CACHE 64u

static uint64_t murmur3_finalize(uint64_t h) {
    h ^= h >> 33;
    h *= UINT64_C(0xFF51AFD7ED558CCD);
    h ^= h >> 33;
    h *= UINT64_C(0xC4CEB9FE1A85EC53);
    h ^= h >> 33;
    return h;
}

static void xorshift128_step(uint64_t s[2]) {
    uint64_t s1 = s[0];
    uint64_t s0 = s[1];
    s[0] = s0;
    s1 ^= s1 << 23;
    s1 ^= s1 >> 17;
    s1 ^= s0;
    s1 ^= s0 >> 26;
    s[1] = s1;
}

/* The step before: s[0] was the old s1; undo the xors of the new s1. */
static void xorshift128_back(uint64_t s[2]) {
    uint64_t old1 = s[0];
    uint64_t t = s[1] ^ old1 ^ (old1 >> 26);
    t ^= (t >> 17) ^ (t >> 34) ^ (t >> 51);
    t ^= (t << 23) ^ (t << 46);
    s[0] = t;
    s[1] = old1;
}

void sjs_random_seed(sjs_random_generator *g, int64_t seed) {
    uint64_t u = (uint64_t)seed;
    g->v8 = (sjs_random){ { murmur3_finalize(u), murmur3_finalize(~u) }, 0 };
    g->cursor[0] = g->v8.state[0];
    g->cursor[1] = g->v8.state[1];
}

bool sjs_random_restore(sjs_random_generator *g, const sjs_random *state) {
    if ((state->state[0] | state->state[1]) == 0 || state->left > RANDOM_CACHE) return false;
    g->v8 = *state;
    g->cursor[0] = state->state[0];
    g->cursor[1] = state->state[1];
    if (state->left > 0) {
        for (uint32_t i = state->left; i < RANDOM_CACHE; i++) xorshift128_back(g->cursor);
    }
    return true;
}

double sjs_random_next(sjs_random_generator *g) {
    if (g->v8.left == 0) {
        for (uint32_t i = 0; i < RANDOM_CACHE; i++) xorshift128_step(g->v8.state);
        g->v8.left = RANDOM_CACHE;
        g->cursor[0] = g->v8.state[0];
        g->cursor[1] = g->v8.state[1];
    }
    double x = sjs_bits_double((g->cursor[0] >> 12) | UINT64_C(0x3FF0000000000000)) - 1;
    xorshift128_back(g->cursor);
    g->v8.left--;
    return x;
}
