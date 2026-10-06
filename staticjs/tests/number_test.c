/*
 * Numbers without a C library: formatting, remainder and rounding.
 *
 *   number_test                 known cases
 *   number_test --print COUNT   lines for diff-node to compare with Node, each
 *                               tagged with what it checks:
 *     F <bits> <text>           COUNT pseudo-random doubles formatted: String(x)
 *     D <bits> <decimal>        COUNT decimal strings read: Number(decimal);
 *                               random digits and exponents, long ones, and
 *                               the exact midpoints between two doubles, a
 *                               digit under and a digit over them
 *     N <bits> <bits> <text>    COUNT strings read: Number(text), parseFloat(text);
 *                               those decimals with white space, a sign, a
 *                               prefix, Infinity or junk around them
 *     I <radix> <bits> <text>   COUNT strings read: parseInt(text, radix), in
 *                               every radix, with up to 400 digits
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/internal.h"

static double from_bits(uint64_t u) {
    double d;
    memcpy(&d, &u, sizeof d);
    return d;
}

static uint64_t to_bits(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static int failures = 0;

static void expect_format(double value, const char *want) {
    char text[32];
    uint32_t n = sjs_number_format(value, text);
    text[n] = '\0';
    if (strcmp(text, want) != 0) {
        fprintf(stderr, "format %016llx: want %s, got %s\n", (unsigned long long)to_bits(value), want, text);
        failures++;
    }
}

static void expect_number(const char *what, double got, double want) {
    if (to_bits(got) != to_bits(want) && !(got != got && want != want)) {
        fprintf(stderr, "%s: want %.17g, got %.17g\n", what, want, got);
        failures++;
    }
}

/* A small PCG generator: the same doubles on every machine. */
static uint64_t state = 0x853c49e6748fea9bull;

static uint64_t next(void) {
    uint64_t old = state;
    state = old * 6364136223846793005ull + 1442695040888963407ull;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    uint32_t low = (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    return ((uint64_t)low << 32) | (uint32_t)(old * 2862933555777941757ull >> 32);
}

static void print_formats(long count) {
    for (long i = 0; i < count; i++) {
        uint64_t bits = next();
        if (i % 4 == 1) bits = to_bits((double)(int64_t)(next() % 2000001) / 1000.0 - 1000.0);
        if (i % 4 == 2) bits = to_bits((double)(next() % 100000) * (i % 3 == 0 ? 1e-5 : 1e5));
        char text[32];
        uint32_t n = sjs_number_format(from_bits(bits), text);
        if (sjs_number_is_negative_zero(from_bits(bits))) {
            memcpy(text, "-0", 2);
            n = 2;
        }
        text[n] = '\0';
        printf("F %016llx %s\n", (unsigned long long)bits, text);
    }
}

/* A decimal string being built: digits, then an exponent. */
#define DECIMAL_ROOM 4096

typedef struct {
    char text[DECIMAL_ROOM];
    size_t length;
} decimal_text;

static void put(decimal_text *t, char c) {
    if (t->length + 1 < DECIMAL_ROOM) t->text[t->length++] = c;
}

static void put_digits(decimal_text *t, size_t count) {
    for (size_t i = 0; i < count; i++) put(t, (char)('0' + next() % 10));
}

static void put_exponent(decimal_text *t, int exponent) {
    char e[16];
    int n = snprintf(e, sizeof e, "e%d", exponent);
    for (int i = 0; i < n; i++) put(t, e[i]);
}

/* The exact decimal of m x 2^e (m < 2^54), in base 10^9 limbs. */
static void exact_decimal(decimal_text *t, uint64_t m, int e) {
    static uint32_t limbs[200];
    size_t n = 0;
    while (m != 0) {
        limbs[n++] = (uint32_t)(m % 1000000000u);
        m /= 1000000000u;
    }
    /* Times 2^e or 5^-e, 2^29 or 5^13 at a time. */
    for (int left = e >= 0 ? e : -e; left > 0;) {
        int step = left < (e >= 0 ? 29 : 13) ? left : (e >= 0 ? 29 : 13);
        uint64_t factor = 1;
        for (int k = 0; k < step; k++) factor *= e >= 0 ? 2u : 5u;
        left -= step;
        uint64_t carry = 0;
        for (size_t k = 0; k < n; k++) {
            uint64_t p = (uint64_t)limbs[k] * factor + carry;
            limbs[k] = (uint32_t)(p % 1000000000u);
            carry = p / 1000000000u;
        }
        for (; carry != 0; carry /= 1000000000u) limbs[n++] = (uint32_t)(carry % 1000000000u);
    }
    char digits[16];
    int d = snprintf(digits, sizeof digits, "%u", limbs[n - 1]);
    for (int i = 0; i < d; i++) put(t, digits[i]);
    for (size_t k = n - 1; k-- > 0;) {
        snprintf(digits, sizeof digits, "%09u", limbs[k]);
        for (int i = 0; i < 9; i++) put(t, digits[i]);
    }
    if (e < 0) put_exponent(t, e);
}

/* A positive finite double, its exponent spread evenly. */
static double random_double(void) {
    for (;;) {
        double x = from_bits(next() & 0x7FFFFFFFFFFFFFFFull);
        if (sjs_number_is_finite(x) && x != 0) return x;
    }
}

static void random_decimal(decimal_text *t, long i) {
    t->length = 0;
    switch (i % 8) {
    case 0: case 1: { /* a few digits, a point somewhere, an exponent */
        size_t count = 1 + next() % 20;
        size_t point = next() % (count + 1);
        for (size_t k = 0; k < count; k++) {
            if (k == point && k != 0) put(t, '.');
            put(t, (char)('0' + next() % 10));
        }
        put_exponent(t, (int)(next() % 700) - 350);
        break;
    }
    case 2: /* 17 significant digits: what a double prints as at most */
    case 3: {
        char text[40];
        snprintf(text, sizeof text, "%.16e", random_double());
        for (size_t k = 0; text[k] != '\0'; k++) put(t, text[k]);
        break;
    }
    case 4: { /* many digits */
        put_digits(t, 20 + next() % (next() % 4 == 0 ? 1200 : 60));
        put_exponent(t, (int)(next() % 800) - 600);
        break;
    }
    default: { /* the midpoint between a double and the next, exactly, or a last digit off */
        double x = random_double();
        uint64_t u = to_bits(x);
        uint64_t field = (u >> 52) & 0x7FFu;
        uint64_t m = (u & ((1ull << 52) - 1u)) | (field != 0 ? 1ull << 52 : 0);
        int e = field != 0 ? (int)field - 1075 : -1074;
        exact_decimal(t, 2 * m + 1, e - 1);
        if (i % 8 == 6) {
            /* under: the last digit 5 of a fraction made 4, or an integer's last digit dropped */
            size_t at = t->length;
            for (size_t k = 0; k < t->length; k++) {
                if (t->text[k] == 'e') at = k;
            }
            if (at > 1 && at < t->length && t->text[at - 1] == '5') t->text[at - 1] = '4';
            else if (at == t->length) t->text[t->length - 1] = t->text[t->length - 1] == '0' ? '0' : (char)(t->text[t->length - 1] - 1);
        } else if (i % 8 == 7) {
            /* over: a 1 far past the midpoint's digits */
            size_t at = t->length;
            for (size_t k = 0; k < t->length; k++) {
                if (t->text[k] == 'e') at = k;
            }
            char tail[16] = "";
            size_t tail_length = t->length - at;
            memcpy(tail, t->text + at, tail_length);
            t->length = at;
            if (tail_length == 0) put(t, '.');
            for (int k = 0; k < 30; k++) put(t, '0');
            put(t, '1');
            for (size_t k = 0; k < tail_length; k++) put(t, tail[k]);
        }
        break;
    }
    }
    t->text[t->length] = '\0';
}

static void print_decimals(long count) {
    static decimal_text t;
    for (long i = 0; i < count; i++) {
        random_decimal(&t, i);
        /* The digits and the exponent apart, as the lexer has them. */
        char *e = strchr(t.text, 'e');
        long exponent = e != NULL ? strtol(e + 1, NULL, 10) : 0;
        size_t length = e != NULL ? (size_t)(e - t.text) : t.length;
        char *point = memchr(t.text, '.', length);
        if (point != NULL) exponent -= (long)(length - (size_t)(point - t.text) - 1);
        double x = sjs_number_from_decimal((sjs_str){ t.text, (uint32_t)length }, exponent);
        printf("D %016llx %s\n", (unsigned long long)to_bits(x), t.text);
    }
}

static void put_text(decimal_text *t, const char *text) {
    for (size_t i = 0; text[i] != '\0'; i++) put(t, text[i]);
}

static const char *const SPACES[] = { " ", "\t", "\v", "\f", "\xc2\xa0", "\xe3\x80\x80", "\xef\xbb\xbf", "\xe2\x80\xa8", "\xe1\xa0\x8e" };
static const char *const JUNK[] = { "x", "e", "e+", "e-", ".", "..", "_1", " 5", "abc", "Infinity", "0x1", "-", "1", "E5" };

/* White space, from what JavaScript trims (and U+180E, which it does not). */
static void put_spaces(decimal_text *t) {
    for (uint64_t n = next() % 4; n > 0 && next() % 2 == 0; n--) put_text(t, SPACES[next() % (sizeof SPACES / sizeof SPACES[0])]);
}

static void put_radix_digits(decimal_text *t, uint32_t radix, size_t count) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (size_t i = 0; i < count; i++) {
        uint32_t d = (uint32_t)(next() % radix);
        put(t, d >= 10 && next() % 2 == 0 ? digits[d + 26] : digits[d]);
    }
}

static void print_strings(long count) {
    static decimal_text body;
    static decimal_text t;
    for (long i = 0; i < count; i++) {
        t.length = 0;
        put_spaces(&t);
        if (next() % 3 == 0) put(&t, next() % 2 == 0 ? '-' : '+');
        switch (next() % 6) {
        case 0: case 1: case 2:
            random_decimal(&body, i);
            put_text(&t, body.text);
            break;
        case 3: {
            static const char *const prefixes[] = { "0x", "0X", "0o", "0O", "0b", "0B" };
            static const uint32_t radixes[] = { 16, 16, 8, 8, 2, 2 };
            uint32_t k = (uint32_t)(next() % 6);
            put_text(&t, prefixes[k]);
            put_radix_digits(&t, radixes[k], (size_t)(next() % (next() % 8 == 0 ? 300 : 24)));
            break;
        }
        case 4:
            put_text(&t, next() % 4 == 0 ? "infinity" : "Infinity");
            break;
        default:
            put_text(&t, JUNK[next() % (sizeof JUNK / sizeof JUNK[0])]);
            break;
        }
        if (next() % 4 == 0) put_text(&t, JUNK[next() % (sizeof JUNK / sizeof JUNK[0])]);
        put_spaces(&t);
        t.text[t.length] = '\0';
        sjs_str text = { t.text, (uint32_t)t.length };
        printf("N %016llx %016llx %s\n", (unsigned long long)to_bits(sjs_number_parse(text)),
               (unsigned long long)to_bits(sjs_number_parse_float(text)), t.text);
    }
}

static void print_integers(long count) {
    static decimal_text t;
    for (long i = 0; i < count; i++) {
        t.length = 0;
        int32_t radix = (int32_t)(next() % 40) - 1; /* -1 to 38: 0, 1 and 37 and up too */
        uint32_t digits_radix = radix >= 2 && radix <= 36 ? (uint32_t)radix : 10u;
        put_spaces(&t);
        if (next() % 3 == 0) put(&t, next() % 2 == 0 ? '-' : '+');
        if ((radix == 0 || radix == 16) && next() % 3 == 0) {
            put_text(&t, next() % 2 == 0 ? "0x" : "0X");
            digits_radix = 16;
        }
        if (next() % 4 == 0) put_radix_digits(&t, 1, (size_t)(next() % 30)); /* leading zeros */
        put_radix_digits(&t, digits_radix, (size_t)(next() % (next() % 8 == 0 ? 400 : 40)));
        if (next() % 4 == 0) put_text(&t, JUNK[next() % (sizeof JUNK / sizeof JUNK[0])]);
        t.text[t.length] = '\0';
        double x = sjs_number_parse_int((sjs_str){ t.text, (uint32_t)t.length }, radix);
        printf("I %d %016llx %s\n", (int)radix, (unsigned long long)to_bits(x), t.text);
    }
}

static int print(long count) {
    print_formats(count);
    print_decimals(count);
    print_strings(count);
    print_integers(count);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--print") == 0) return print(atol(argv[2]));

    expect_format(0.0, "0");
    expect_format(-0.0, "0");
    expect_format(1.0, "1");
    expect_format(-1.5, "-1.5");
    expect_format(0.1, "0.1");
    expect_format(0.1 + 0.2, "0.30000000000000004");
    expect_format(1e21, "1e+21");
    expect_format(1e20, "100000000000000000000");
    expect_format(123456789012345680000.0, "123456789012345680000");
    expect_format(1e-6, "0.000001");
    expect_format(1e-7, "1e-7");
    expect_format(1.5e-7, "1.5e-7");
    expect_format(from_bits(1), "5e-324");
    expect_format(from_bits(0x7FEFFFFFFFFFFFFFull), "1.7976931348623157e+308");
    expect_format(from_bits(0x0010000000000000ull), "2.2250738585072014e-308");
    expect_format(9007199254740992.0, "9007199254740992");
    expect_format(9007199254740994.0, "9007199254740994");
    expect_format(4.35, "4.35");
    expect_format(1.0 / 0.0, "Infinity");
    expect_format(-1.0 / 0.0, "-Infinity");
    expect_format(0.0 / 0.0, "NaN");
    expect_format(100.0, "100");
    expect_format(1234.5678, "1234.5678");

    expect_number("7 % 3", sjs_number_remainder(7, 3), 1);
    expect_number("-7 % 3", sjs_number_remainder(-7, 3), -1);
    expect_number("5.5 % 2", sjs_number_remainder(5.5, 2), 1.5);
    expect_number("-0 % 1", sjs_number_remainder(-0.0, 1), -0.0);
    expect_number("1 % Infinity", sjs_number_remainder(1, 1.0 / 0.0), 1);
    expect_number("subnormal % subnormal", sjs_number_remainder(from_bits(7), from_bits(3)), from_bits(1));
    expect_number("floor(-0.5)", sjs_number_floor(-0.5), -1);
    expect_number("floor(-0)", sjs_number_floor(-0.0), -0.0);
    expect_number("ceil(-0.5)", sjs_number_ceil(-0.5), -0.0);
    expect_number("round(2.5)", sjs_number_round(2.5), 3);
    expect_number("round(-2.5)", sjs_number_round(-2.5), -2);
    expect_number("round(-0.4)", sjs_number_round(-0.4), -0.0);
    expect_number("round(0.49999999999999994)", sjs_number_round(0.49999999999999994), 0);
    expect_number("trunc(-1.7)", sjs_number_trunc(-1.7), -1);
    expect_number("trunc(1e300)", sjs_number_trunc(1e300), 1e300);

    expect_number("15e-1", sjs_number_from_decimal((sjs_str){ "15", 2 }, -1), 1.5);
    expect_number("1.5 with its point", sjs_number_from_decimal((sjs_str){ "1.5", 3 }, -1), 1.5);
    expect_number("20 digits", sjs_number_from_decimal((sjs_str){ "12345678901234567890", 20 }, 0), 12345678901234567000.0);
    expect_number("5e-324", sjs_number_from_decimal((sjs_str){ "5", 1 }, -324), from_bits(1));
    expect_number("2e-324", sjs_number_from_decimal((sjs_str){ "2", 1 }, -324), 0);
    expect_number("3e-324", sjs_number_from_decimal((sjs_str){ "3", 1 }, -324), from_bits(1));
    expect_number("1e309", sjs_number_from_decimal((sjs_str){ "1", 1 }, 309), 1.0 / 0.0);
    expect_number("0.000", sjs_number_from_decimal((sjs_str){ "0.000", 5 }, -3), 0);
    /* Expected values printed by Node 22. */
    expect_number("0x1fffffffffffff1", sjs_number_from_binary((sjs_str){ "1fffffffffffff1", 15 }, 16), 144115188075855860.0);
    expect_number("0x1fffffffffffff8, a tie", sjs_number_from_binary((sjs_str){ "1fffffffffffff8", 15 }, 16), 144115188075855870.0);

    if (failures != 0) {
        fprintf(stderr, "number_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("number_test: formatting, remainder and rounding agree with JavaScript\n");
    return 0;
}
