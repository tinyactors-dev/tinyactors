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
 *                               digit under and a digit over them, and ties
 *                               of up to 19 digits next to 2^53
 *     N <bits> <bits> <text>    COUNT strings read: Number(text), parseFloat(text);
 *                               those decimals with white space, a sign, a
 *                               prefix, Infinity or junk around them
 *     I <radix> <bits> <text>   COUNT strings read: parseInt(text, radix), in
 *                               every radix, with up to 400 digits
 *     S <x> <sqrt> <fround>     COUNT doubles: Math.sqrt(x), Math.fround(x); any
 *                               bits, and float32 midpoints and their neighbours
 *     H <hypot> <x>...          COUNT argument lists: Math.hypot(...x); up to 8
 *                               arguments, of any bits, of near magnitudes,
 *                               integers, zeros, Infinity and NaN
 *     C <a> <b> <clz32> <imul>  COUNT pairs: Math.clz32(a), Math.imul(a, b)
 *     B <op> <a> <b> <result>   COUNT BigInt operations: + - * / % ** (^) and a
 *                               comparison (<), on 1 to 20 limbs, in hex
 *     T <a> <radix> <dec> <s>   COUNT BigInts as text: String(a), toString(radix)
 *     M <a> <bits> <c>          COUNT BigInts compared with a double
 *     R <a> <bits>              COUNT BigInts as numbers: Number(a)
 *     A <n> <a> <int> <uint>    COUNT BigInts wrapped: BigInt.asIntN(n, a), asUintN
 *     P <m> <bits> <d> <text>   a fixed list of doubles, whatever COUNT is, through
 *                               toFixed(d) (m f), toExponential(d) (e) and
 *                               toPrecision(d) (p); d u for undefined
 *     Q <radix> <bits> <text>   a fixed list of doubles, whatever COUNT is, in every
 *                               radix 2 to 36: x.toString(radix)
 *     W <x> <y> <x ** y>        a fixed list of pairs, whatever COUNT is: Math.pow(x, y)
 *                               and x ** y (V8's fdlibm pow, src/math.c)
 *     U <name> <x> <f(x)>       a fixed list of doubles, whatever COUNT is, through each
 *                               one-argument Math function of src/math.c: Math[name](x)
 *     V <y> <x> <atan2(y, x)>   a fixed list of pairs, whatever COUNT is: Math.atan2(y, x)
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

static void expect_radix(double value, uint32_t radix, const char *want) {
    static char text[SJS_NUMBER_RADIX_MAX + 1u];
    uint32_t n = sjs_number_format_radix(value, radix, text);
    text[n] = '\0';
    if (strcmp(text, want) != 0) {
        fprintf(stderr, "toString(%u) of %016llx: want %s, got %s\n", radix, (unsigned long long)to_bits(value), want, text);
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
    case 3: { /* m + j / 2^k next to 2^53: up to 19 digits over 10^k, half of them ties */
        int k = 1 + (int)(next() % 3);
        uint64_t m = (1ull << (53 - k)) + next() % (1ull << (53 - k));
        exact_decimal(t, (m << k) | (next() % (1ull << k)), -k);
        break;
    }
    case 2: { /* 17 significant digits: what a double prints as at most */
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

/* A double of any bits, NaN and the infinities included. */
static double any_double(void) {
    switch (next() % 16) {
    case 0: return 0.0 / 0.0;
    case 1: return next() % 2 == 0 ? 1.0 / 0.0 : -1.0 / 0.0;
    case 2: return next() % 2 == 0 ? 0.0 : -0.0;
    case 3: return from_bits(next() & 0x800FFFFFFFFFFFFFull); /* subnormal */
    default: return from_bits(next());
    }
}

static void print_roots(long count) {
    for (long i = 0; i < count; i++) {
        double x = any_double();
        if (i % 2 == 1) {
            /* Halfway between two floats, or a double on either side of it. */
            uint32_t f = (uint32_t)next() & 0x7F7FFFFFu;
            float low;
            float high;
            uint32_t g = f + 1u;
            memcpy(&low, &f, sizeof low);
            memcpy(&high, &g, sizeof high);
            x = ((double)low + (double)high) / 2;
            if (i % 6 == 3) x = from_bits(to_bits(x) + 1u);
            if (i % 6 == 5) x = from_bits(to_bits(x) - 1u);
            if (next() % 2 == 0) x = -x;
        }
        printf("S %016llx %016llx %016llx\n", (unsigned long long)to_bits(x),
               (unsigned long long)to_bits(sjs_number_sqrt(x)), (unsigned long long)to_bits(sjs_number_fround(x)));
    }
}

static void print_hypots(long count) {
    double args[8];
    for (long i = 0; i < count; i++) {
        uint32_t n = (uint32_t)(next() % 9);
        if (i % 4 != 0 && n < 2) n += 2;
        int base = (int)(next() % 2046) + 1; /* a field for numbers of near magnitude */
        for (uint32_t k = 0; k < n; k++) {
            switch ((i + k) % 5) {
            case 0:
                args[k] = next() % 64 == 0 ? any_double() : from_bits(next());
                break;
            case 1: case 2: {
                int field = base + (int)(next() % 7) - 3;
                if (field < 1) field = 1;
                if (field > 2046) field = 2046;
                args[k] = from_bits(((uint64_t)field << 52) | (next() & 0x800FFFFFFFFFFFFFull));
                break;
            }
            case 3:
                args[k] = (double)(int64_t)(next() % 2001) - 1000.0;
                break;
            default:
                args[k] = next() % 16 == 0 ? any_double() : (double)(next() % 100000) / 1000.0;
                break;
            }
        }
        sjs_hypot h = { 0, 0, 0, false };
        for (uint32_t k = 0; k < n; k++) sjs_hypot_see(&h, args[k]);
        double result;
        if (!sjs_hypot_settled(&h, &result)) {
            for (uint32_t k = 0; k < n; k++) sjs_hypot_add(&h, args[k]);
            result = sjs_hypot_end(&h);
        }
        printf("H %016llx", (unsigned long long)to_bits(result));
        for (uint32_t k = 0; k < n; k++) printf(" %016llx", (unsigned long long)to_bits(args[k]));
        printf("\n");
    }
}

static double any_integer(void) {
    switch (next() % 4) {
    case 0: return any_double();
    case 1: return (double)(int32_t)next();
    case 2: return (double)(uint32_t)next() + (double)(next() % 4) * 4294967296.0 + (next() % 2 == 0 ? 0.5 : 0);
    default: return -(double)(next() % 100000) / 7.0;
    }
}

static void print_integer_ops(long count) {
    for (long i = 0; i < count; i++) {
        double a = any_integer();
        double b = any_integer();
        uint32_t bits = sjs_number_to_uint32(a);
        uint32_t product = bits * sjs_number_to_uint32(b);
        printf("C %016llx %016llx %d %d\n", (unsigned long long)to_bits(a), (unsigned long long)to_bits(b),
               bits == 0 ? 32 : __builtin_clz(bits), product >= 0x80000000u ? (int)(product - 0x80000000u) - 0x7FFFFFFF - 1 : (int)product);
    }
}

/* ------------------------------------------------------------------------
 * BigInts, against Node: random operands of 1 to 20 limbs, each limb
 * random or one of the edges (0, 1, 2^31, 2^32 - 1), so that carries and
 * borrows cross limbs and quotient estimates are corrected. Written in
 * hexadecimal, a sign before it: Node reads them with BigInt("0x...").
 */

static void *malloc_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    return malloc(size);
}

static void malloc_free(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const sjs_allocator_vtable malloc_vtable = { malloc_alloc, malloc_free };

static sjs_context *bigint_context(void) {
    static sjs_context *context;
    if (context == NULL) {
        sjs_context_config config = { { NULL, &malloc_vtable }, SJS_LIMITS_DEFAULT };
        if (sjs_context_init(&context, &config) != SJS_OK) abort();
    }
    return context;
}

typedef struct {
    char text[20 * 8 + 2];
} bigint_hex;

static bigint_hex random_hex(uint32_t limbs) {
    bigint_hex h;
    size_t at = 0;
    if (next() % 2 == 0) h.text[at++] = '-';
    for (uint32_t i = 0; i < limbs; i++) {
        uint32_t limb;
        switch (next() % 8) {
        case 0: limb = 0; break;
        case 1: limb = 0xffffffffu; break;
        case 2: limb = 0x80000000u; break;
        case 3: limb = 1; break;
        default: limb = (uint32_t)next(); break;
        }
        if (i == 0 && limb == 0) limb = 1u + (uint32_t)(next() % 0xfffffffeu);
        at += (size_t)snprintf(h.text + at, sizeof h.text - at, "%08x", limb);
    }
    return h;
}

static sjs_value bigint_from_hex(const char *text) {
    bool negative = text[0] == '-';
    const char *digits = text + (negative ? 1 : 0);
    sjs_value v;
    if (sjs_bigint_from_digits(bigint_context(), (sjs_bigint_digits){ { digits, (uint32_t)strlen(digits) }, 16u, negative }, &v) != SJS_OK) abort();
    return v;
}

static void print_bigint(sjs_value v, uint32_t radix) {
    sjs_buf text = { 0 };
    sjs_context *context = bigint_context();
    if (sjs_bigint_format(&context->counting, (sjs_bigint_radix){ SJS_AS_BIGINT(v), radix }, &text) != SJS_OK) abort();
    printf("%.*s", (int)text.length, (const char *)text.ptr);
    sjs_buf_free(&context->counting, &text);
}

static const char BIGINT_OPS[] = { '+', '-', '*', '/', '%', '^', '<' };

/* B <op> <a> <b> <result>: a op b, `^` for **, `<` for the comparison (-1, 0
 * or 1), and `!` where staticjs faults (a division by 0n). */
static void print_bigint_ops(long count) {
    sjs_context *context = bigint_context();
    for (long i = 0; i < count; i++) {
        char op = BIGINT_OPS[i % (long)sizeof BIGINT_OPS];
        bigint_hex a = random_hex(1u + (uint32_t)(next() % 20));
        bigint_hex b = random_hex(1u + (uint32_t)(next() % 20));
        if (op == '^') {
            a = random_hex(1u + (uint32_t)(next() % 4));
            snprintf(b.text, sizeof b.text, "%x", (unsigned)(next() % 48));
        }
        if (next() % 16 == 0) snprintf(b.text, sizeof b.text, "%s", next() % 2 == 0 ? "0" : "1");
        if (next() % 16 == 0) snprintf(a.text, sizeof a.text, "%s", b.text);
        sjs_value x = bigint_from_hex(a.text);
        sjs_value y = bigint_from_hex(b.text);
        printf("B %c %s %s ", op, a.text, b.text);
        if (op == '<') {
            printf("%d\n", sjs_bigint_compare(SJS_AS_BIGINT(x), SJS_AS_BIGINT(y)));
        } else {
            uint8_t codes[] = { SJS_OP_ADD, SJS_OP_SUBTRACT, SJS_OP_MULTIPLY, SJS_OP_DIVIDE, SJS_OP_REMAINDER, SJS_OP_EXPONENT };
            sjs_bigint_op o = { .op = codes[strchr(BIGINT_OPS, op) - BIGINT_OPS], .a = x, .b = y };
            sjs_result r = sjs_bigint_binary(context, &o);
            if (r == SJS_ERR_PROGRAM) {
                printf("!\n");
            } else {
                if (r != SJS_OK) abort();
                print_bigint(o.result, 16);
                printf("\n");
                sjs_release(context, o.result);
            }
        }
        sjs_release(context, x);
        sjs_release(context, y);
    }
}

/* T <a> <radix> <decimal> <text>: String(a) and a.toString(radix), and
 * BigInt(decimal) read back. M <a> <bits> <c>: a compared with a double. */
static void print_bigint_texts(long count) {
    sjs_context *context = bigint_context();
    for (long i = 0; i < count; i++) {
        bigint_hex a = random_hex(1u + (uint32_t)(next() % 20));
        sjs_value x = bigint_from_hex(a.text);
        uint32_t radix = 2u + (uint32_t)(next() % 35);
        sjs_buf decimal = { 0 };
        if (sjs_bigint_format(&context->counting, (sjs_bigint_radix){ SJS_AS_BIGINT(x), 10u }, &decimal) != SJS_OK) abort();
        sjs_bigint_digits digits;
        if (!sjs_bigint_parse((sjs_str){ (const char *)decimal.ptr, decimal.length }, &digits)) abort();
        sjs_value back;
        if (sjs_bigint_from_digits(context, digits, &back) != SJS_OK) abort();
        if (!sjs_strict_equal_primitive(x, back)) {
            fprintf(stderr, "number_test: %s does not read back from its decimal text\n", a.text);
            abort();
        }
        printf("T %s %u %.*s ", a.text, radix, (int)decimal.length, (const char *)decimal.ptr);
        print_bigint(x, radix);
        printf("\n");
        double d;
        switch (next() % 4) {
        case 0: d = any_double(); break;
        case 1: d = (double)(int64_t)next() * (double)(1u << (next() % 16)); break;
        default: d = sjs_bigint_to_number(SJS_AS_BIGINT(x)) + (next() % 2 == 0 ? 0.0 : 0.5); break;
        }
        printf("M %s %016llx %d\n", a.text, (unsigned long long)to_bits(d), sjs_bigint_compare_number(SJS_AS_BIGINT(x), d));
        printf("R %s %016llx\n", a.text, (unsigned long long)to_bits(sjs_bigint_to_number(SJS_AS_BIGINT(x))));
        /* asIntN and asUintN, at widths around the value's and limbs'. */
        uint64_t widths[] = { next() % 70, next() % 700, 32u * (next() % 22), 32u * (next() % 22) + 1u };
        uint64_t bits = widths[next() % 4];
        sjs_value as_int;
        sjs_value as_uint;
        if (sjs_bigint_wrap(context, (sjs_bigint_wrap_params){ x, bits, true }, &as_int) != SJS_OK ||
            sjs_bigint_wrap(context, (sjs_bigint_wrap_params){ x, bits, false }, &as_uint) != SJS_OK) {
            abort();
        }
        printf("A %llu %s ", (unsigned long long)bits, a.text);
        print_bigint(as_int, 16);
        printf(" ");
        print_bigint(as_uint, 16);
        printf("\n");
        sjs_release(context, as_int);
        sjs_release(context, as_uint);
        sjs_buf_free(&context->counting, &decimal);
        sjs_release(context, x);
        sjs_release(context, back);
    }
}

/* toFixed, toExponential and toPrecision of a fixed list of doubles (ties,
 * values just under and over a half, the edges of each form, 1e21,
 * subnormals and the largest double; then -0, NaN and the infinities), each
 * with a fixed list of digit counts: the same lines for any COUNT. */
static const double DIGITS_VALUES[] = {
    0.0, 1.0, 0.5, 1.5, 2.5, -2.5, -1.5, 0.125, 0.375, 1.25, 1.005, 1.045, 1.255, 4.35,
    8.345, 1.45, 10.235, 9.995, 99.995, 0.0005, 0.00005, 0.000001, 0.0000015, 1e-7, 1e-10,
    0.15, 0.35, 0.1, 0.2, 0.3, 0.30000000000000004, 0.3333333333333333, 0.6666666666666666,
    1.0000000000000002, 0.9999999999999999, 123.456, -123.456, 0.000123, 25.0, 1e15,
    999999999999999.9, 4503599627370495.5, 4503599627370497.0, 9007199254740992.0,
    9007199254740994.0, 9007199254740993.0, 18014398509481984.0, 1e20, 1.2345e20,
    99999999999999990000.0, 999999999999999900000.0, 9.999999999999999e20, 1e21,
    1.0000000000000001e21, 1e22, 1e100, 1e300, 1.7976931348623157e308,
    -1.7976931348623157e308, 2.2250738585072014e-308, 2.225073858507201e-308,
    4.9406564584124654e-324, 1e-300, 1e-323, 5e-7, 5e-8, 0.00000049999999999999995,
    0.000000123456789, 123456789.123456789, -0.0001, -0.00001, 0.045, 0.055, 0.0045, 1.0e-5,
    7.0, 12.0, 99.0, 100.0, 999.0, 1000.0, 0.95, 0.995, 0.9995, -0.5, -0.05,
    314159.26535897932, 2.718281828459045, 1.4142135623730951, 6.02214076e23,
    1.602176634e-19, 65536.5, 0.6, 0.7, 0.8, 0.9,
    -0.0, 0.0 / 0.0, 1.0 / 0.0, -1.0 / 0.0
};

static void print_digit_methods(void) {
    static const struct {
        char tag;
        sjs_digits_mode mode;
        int32_t first; /* -1: undefined, then from 0 or 1 */
        int32_t last;
    } METHODS[] = {
        { 'f', SJS_DIGITS_FIXED, 0, 20 },
        { 'e', SJS_DIGITS_EXPONENTIAL, -1, 20 },
        { 'p', SJS_DIGITS_PRECISION, -1, 21 },
    };
    static const int32_t LARGE[] = { 25, 50, 100 };
    for (uint32_t m = 0; m < 3u; m++) {
        for (size_t v = 0; v < sizeof DIGITS_VALUES / sizeof DIGITS_VALUES[0]; v++) {
            int32_t counts[32];
            uint32_t n = 0;
            for (int32_t d = METHODS[m].first; d <= METHODS[m].last; d++) {
                if (d == 0 && METHODS[m].mode == SJS_DIGITS_PRECISION) continue;
                counts[n++] = d;
            }
            for (uint32_t i = METHODS[m].mode == SJS_DIGITS_FIXED ? 0u : 1u; i < 3u; i++) counts[n++] = LARGE[i];
            for (uint32_t i = 0; i < n; i++) {
                char text[SJS_NUMBER_DIGITS_MAX + 1u];
                uint32_t length = sjs_number_format_digits((sjs_number_digits){ DIGITS_VALUES[v], counts[i], METHODS[m].mode }, text);
                text[length] = '\0';
                printf("P %c %016llx ", METHODS[m].tag, (unsigned long long)to_bits(DIGITS_VALUES[v]));
                if (counts[i] < 0) printf("u %s\n", text);
                else printf("%d %s\n", counts[i], text);
            }
        }
    }
}

/* toString(radix) of a fixed list of doubles in every radix 2 to 36:
 * integers to the largest double, fractions to the smallest subnormal,
 * negatives, -0, NaN and the infinities; then, for each radix, its powers
 * and the doubles next to them, a unit off them, and its negative powers
 * and their neighbours. The same lines for any COUNT. */
static const double RADIX_VALUES[] = {
    0.0, 1.0, 2.0, 3.0, 7.0, 9.0, 10.0, 15.0, 16.0, 17.0, 35.0, 36.0, 37.0, 100.0, 255.0, 256.0,
    1000.0, 4095.0, 65535.0, 123456789.0, 1e9, 2147483647.0, 2147483648.0, 4294967295.0,
    4294967296.0, 1e15, 4503599627370495.0, 4503599627370496.0, 9007199254740991.0,
    9007199254740992.0, 9007199254740994.0, 18014398509481984.0, 18014398509481988.0,
    1152921504606846976.0, 9223372036854775808.0, 18446744073709551616.0,
    18446744073709555712.0, 1e18, 1e20, 1e21, 1e22, 1.2345e25, 1e30, 1e100, 1.2345678901234567e200,
    1e300, 1e308, 8.98846567431158e307, 1.7976931348623157e308, 1.7976931348623155e308,
    0.1, 0.2, 0.3, 0.30000000000000004, 0.25, 0.125, 0.5, 0.7, 0.9, 0.3333333333333333,
    0.6666666666666666, 0.14285714285714285, 0.1111111111111111, 0.9999999999999999,
    0.99999999999999994, 1.0000000000000002, 1.5, 2.5, 3.5, 255.5, 4503599627370495.5,
    2251799813685247.75, 1e15 + 0.3, 123.456, 3.141592653589793, 2.718281828459045,
    1.4142135623730951, 0.001, 0.0001, 1e-5, 1e-7, 1e-10, 1e-20, 1e-100, 1e-300,
    2.2250738585072014e-308, 2.225073858507201e-308, 1e-310, 1e-320, 1e-323, 5e-324,
    1.2345e-5, 6.02214076e23, 1.602176634e-19, 0.000001, 0.0000015, 65536.5, 0.05,
    0.95, 0.995, 1.0 / 1024.0, 1.0 / 3.0 + 1.0, 100.0 / 3.0, 2.0 / 3.0 * 1e10,
    -1.0, -2.0, -255.0, -2147483648.0, -9007199254740992.0, -1e21, -1.7976931348623157e308,
    -0.1, -0.5, -0.3333333333333333, -123.456, -255.5, -1e-7, -5e-324, -2.2250738585072014e-308,
    -0.0, 0.0 / 0.0, 1.0 / 0.0, -1.0 / 0.0
};

static void print_radix(uint32_t radix, double value) {
    static char text[SJS_NUMBER_RADIX_MAX + 1u];
    uint32_t length = sjs_number_format_radix(value, radix, text);
    text[length] = '\0';
    printf("Q %u %016llx %s\n", radix, (unsigned long long)to_bits(value), text);
}

static void print_radix_methods(void) {
    static const int32_t POWERS[] = { 1, 2, 5, 10, 13, 20, 40, 100, 200 };
    static const int32_t NEGATIVE[] = { 1, 2, 3, 7, 20, 100 };
    for (uint32_t radix = 2; radix <= 36u; radix++) {
        for (size_t v = 0; v < sizeof RADIX_VALUES / sizeof RADIX_VALUES[0]; v++) print_radix(radix, RADIX_VALUES[v]);
        for (size_t k = 0; k < sizeof POWERS / sizeof POWERS[0]; k++) {
            double p = 1.0;
            for (int32_t i = 0; i < POWERS[k]; i++) p *= (double)radix;
            if (p > 1.7976931348623157e308) continue;
            print_radix(radix, p);
            print_radix(radix, from_bits(to_bits(p) + 1u));
            print_radix(radix, from_bits(to_bits(p) - 1u));
            print_radix(radix, p + 1.0);
            print_radix(radix, p - 1.0);
        }
        for (size_t k = 0; k < sizeof NEGATIVE / sizeof NEGATIVE[0]; k++) {
            double p = 1.0;
            for (int32_t i = 0; i < NEGATIVE[k]; i++) p /= (double)radix;
            print_radix(radix, p);
            print_radix(radix, from_bits(to_bits(p) + 1u));
            print_radix(radix, from_bits(to_bits(p) - 1u));
        }
    }
}

/* x ** y of a fixed list of pairs, the same lines for any COUNT: every
 * base of POW_BASES with every exponent of POW_EXPONENTS (specials,
 * subnormals, the largest and smallest normals, integers small and large,
 * odd and even, halves, values next to 1, and the thresholds of fdlibm's
 * pow: |y| over 2^31 and 2^64, 2^53 where every double is an even
 * integer); then integers to integer powers, 10 and 2 to every power whose
 * result is between overflow and underflow and a little past both, bases
 * next to 1 to huge powers, bases at the edges of pow's intervals of x
 * (sqrt(3/2) and sqrt(3)), and results at the edges of overflow (z = 1024)
 * and underflow (z = -1075) and in the subnormals. */
static const double POW_BASES[] = {
    0.0, -0.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 1.0, -1.0, 2.0, -2.0, 3.0, -3.0, 0.5, -0.5,
    1.5, -1.5, 10.0, -10.0, 0.1, 7.0, 0.25, 1e10, 1e-10, 1e300, -1e300, 1e-300,
    4.9406564584124654e-324, -4.9406564584124654e-324, 2.2250738585072009e-308,
    2.2250738585072014e-308, -2.2250738585072014e-308, 1.7976931348623157e308,
    -1.7976931348623157e308, 1.0000000000000002, 0.9999999999999999, -1.0000000000000002,
    -0.9999999999999999, 1.0000009536743164, 0.9999990463256836, 1.2247448713915890,
    1.7320508075688772, 2.718281828459045, 3.141592653589793, -3.141592653589793,
    123456.789, 65536.0, 4294967296.0, 9007199254740992.0, 0.7, 1.1, 0.9, 100.0, -7.0,
};
static const double POW_EXPONENTS[] = {
    0.0, -0.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 3.0, -3.0,
    1.5, -1.5, 0.25, 10.0, -10.0, 0.1, 1.0 / 3.0, 52.0, 53.0, 100.0, -100.0, 1023.0,
    1024.0, -1022.0, -1074.0, -1075.0, 2147483647.0, 2147483648.0, 2147483649.0,
    -2147483649.0, 4294967297.0, 4503599627370497.0, 9007199254740991.0, 9007199254740992.0,
    -9007199254740991.0, 18446744073709551616.0, 36893488147419103232.0,
    -36893488147419103232.0, 1e300, -1e300, 1.7976931348623157e308, 4.9406564584124654e-324,
    -4.9406564584124654e-324, 2.5, -2.5, 0.75, 1e-10, 31.0, 1000000.5,
};

static void print_power(double x, double y) {
    printf("W %016llx %016llx %016llx\n", (unsigned long long)to_bits(x), (unsigned long long)to_bits(y),
           (unsigned long long)to_bits(sjs_math_pow(x, y)));
}

static void print_powers(void) {
    for (size_t i = 0; i < sizeof POW_BASES / sizeof POW_BASES[0]; i++) {
        for (size_t k = 0; k < sizeof POW_EXPONENTS / sizeof POW_EXPONENTS[0]; k++) print_power(POW_BASES[i], POW_EXPONENTS[k]);
    }
    for (int x = -12; x <= 12; x++) {
        for (int y = -6; y <= 40; y++) print_power((double)x, (double)y);
    }
    for (int y = -330; y <= 312; y++) print_power(10.0, (double)y);
    for (int y = -1080; y <= 1030; y++) print_power(2.0, (double)y);
    for (int y = -1080; y <= 1030; y += 7) print_power(-2.0, (double)y + 0.5 * (double)(y % 2));
    for (int y = -330; y <= 312; y += 11) print_power(-10.0, (double)y);
    static const double HUGE_EXPONENTS[] = {
        2147483649.0, -2147483649.0, 4294967296.0, 1e10, -1e10, 1e15, 4503599627370497.0,
        9007199254740992.0, 18446744073709551616.0, 18446744073709555712.0,
    };
    for (uint64_t ulps = 1; ulps <= 1u << 20; ulps *= 2) {
        for (size_t k = 0; k < sizeof HUGE_EXPONENTS / sizeof HUGE_EXPONENTS[0]; k++) {
            print_power(from_bits(to_bits(1.0) + ulps), HUGE_EXPONENTS[k]);
            print_power(from_bits(to_bits(1.0) - ulps), HUGE_EXPONENTS[k]);
            print_power(-from_bits(to_bits(1.0) + ulps), HUGE_EXPONENTS[k]);
        }
    }
    /* High words at the edges of pow's intervals 0x3988E and 0xBB67A. */
    static const uint64_t EDGES[] = { 0x3ff3988e00000000ull, 0x3ff3988effffffffull, 0x3ff3988f00000000ull,
                                      0x3ffbb67900000000ull, 0x3ffbb679ffffffffull, 0x3ffbb67a00000000ull };
    static const double EDGE_EXPONENTS[] = { 0.5, 3.0, -3.0, 0.1, 1.5, 100.5, -1000.0, 1e-5 };
    for (size_t i = 0; i < sizeof EDGES / sizeof EDGES[0]; i++) {
        for (size_t k = 0; k < sizeof EDGE_EXPONENTS / sizeof EDGE_EXPONENTS[0]; k++) {
            print_power(from_bits(EDGES[i]), EDGE_EXPONENTS[k]);
            print_power(from_bits(EDGES[i] - 0x0010000000000000ull), EDGE_EXPONENTS[k]); /* the same, halved */
        }
    }
    /* Results next to overflow (z = 1024) and underflow (z = -1075), and
     * in the subnormals: bases a few units from 2 and 0.5. */
    static const double EDGE_POWERS[] = { 1023.0, 1024.0, 1023.5, -1022.0, -1073.0, -1074.0, -1074.5, -1075.0, -1076.0 };
    for (int d = -3; d <= 3; d++) {
        for (size_t k = 0; k < sizeof EDGE_POWERS / sizeof EDGE_POWERS[0]; k++) {
            print_power(from_bits(to_bits(2.0) + (uint64_t)(int64_t)d), EDGE_POWERS[k]);
            print_power(from_bits(to_bits(0.5) + (uint64_t)(int64_t)d), -EDGE_POWERS[k]);
            print_power(-from_bits(to_bits(2.0) + (uint64_t)(int64_t)d), EDGE_POWERS[k]);
        }
    }
}

/* Math.exp, expm1, log, log1p, log2 and log10 of a fixed list of doubles,
 * the same lines for any COUNT, each function given every input (8,391
 * each): specials (zeros, infinities, NaN, subnormals, the largest and
 * smallest normals); the thresholds of fdlibm's ranges and 4 units either
 * side of each, of either sign: exp's and expm1's overflow (709.78) and
 * underflow (-745.13), 0.5 ln2 and 1.5 ln2, 2^-28, 2^-54, 56 ln2 and
 * the scale of k = -1021; the logs' sqrt(2) and sqrt(2)/2, the edges of
 * log's polynomial choice (0x6147A, 0x6B851), log1p's 1 + x < sqrt(2)
 * (0x3FDA827A and 0xBFD2BEC4), 2^-29, 2^-54 and 2^53; integers from -1100
 * to 1100 and halves; every power of two and its neighbours, powers of
 * ten; 1 +- 2^-j and -1 + 2^-j; and every multiple of ln2 from -1080 to
 * 1030, where exp's k changes. */
static const uint64_t UNARY_EDGES[] = {
    0x40862E42FEFA39EFull, 0x40874910D52D3051ull, 0x40862E4200000000ull, 0x3FD62E4200000000ull,
    0x3FD62E42FEFA39EFull, 0x3FF0A2B200000000ull, 0x3FF0A2B23F3BAB73ull, 0x3E30000000000000ull,
    0x3C90000000000000ull, 0x4043687A00000000ull, 0x4043687A9F1AF2B1ull, 0x4086232BDD7ABCD2ull,
    0x40862B7D369A5AA9ull, 0x3FF6A09E667F3BCDull, 0x3FE6A09E667F3BCDull, 0x3FF6A09E00000000ull,
    0x3FF95F6400000000ull, 0x3FE95F6400000000ull, 0x3FF6147A00000000ull, 0x3FF6B85100000000ull,
    0x3FF0000000000000ull, 0x3FEFFFFF00000000ull, 0x3FF0000100000000ull, 0x0010000000000000ull,
    0x7FEFFFFFFFFFFFFFull, 0x3FDA827A00000000ull, 0x3FD2BEC400000000ull, 0x3E20000000000000ull,
    0x4340000000000000ull, 0x3FDA827999FCEF32ull, 0x3FD2BEC333018866ull, 0x4005BF0A8B145769ull,
};

/* The exp and log family first (EXP_LOG_COUNT of them), then the
 * trigonometric functions (to TRIG_END), then the hyperbolic ones and
 * cbrt, each group given its own list. */
static const char *const UNARY_NAMES[] = {
    "exp", "expm1", "log", "log1p", "log2", "log10", "sin", "cos", "tan", "asin", "acos", "atan",
    "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "cbrt",
};
#define EXP_LOG_COUNT 6
#define TRIG_END 12

static double unary(size_t f, double x) {
    switch (f) {
    case 0: return sjs_math_exp(x);
    case 1: return sjs_math_expm1(x);
    case 2: return sjs_math_log(x);
    case 3: return sjs_math_log1p(x);
    case 4: return sjs_math_log2(x);
    case 5: return sjs_math_log10(x);
    case 6: return sjs_math_sin(x);
    case 7: return sjs_math_cos(x);
    case 8: return sjs_math_tan(x);
    case 9: return sjs_math_asin(x);
    case 10: return sjs_math_acos(x);
    case 11: return sjs_math_atan(x);
    case 12: return sjs_math_sinh(x);
    case 13: return sjs_math_cosh(x);
    case 14: return sjs_math_tanh(x);
    case 15: return sjs_math_asinh(x);
    case 16: return sjs_math_acosh(x);
    case 17: return sjs_math_atanh(x);
    default: return sjs_math_cbrt(x);
    }
}

static void print_unary_range(size_t first, size_t end, double x) {
    for (size_t f = first; f < end; f++) {
        printf("U %s %016llx %016llx\n", UNARY_NAMES[f], (unsigned long long)to_bits(x),
               (unsigned long long)to_bits(unary(f, x)));
    }
}

static void print_unary_input(double x) {
    print_unary_range(0, EXP_LOG_COUNT, x);
}

static void print_trig_input(double x) {
    print_unary_range(EXP_LOG_COUNT, TRIG_END, x);
}

static void print_hyperbolic_input(double x) {
    print_unary_range(TRIG_END, sizeof UNARY_NAMES / sizeof UNARY_NAMES[0], x);
}

static void print_unary(void) {
    static const double SPECIALS[] = {
        0.0, -0.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 4.9406564584124654e-324, -4.9406564584124654e-324,
        2.2250738585072009e-308, -2.2250738585072009e-308, 2.2250738585072014e-308, -2.2250738585072014e-308,
        1.7976931348623157e308, -1.7976931348623157e308, 1e-300, 1e300, -1e300, 0.1, -0.1, 0.25, 0.75,
        2.718281828459045, 3.141592653589793, 0.6931471805599453, 2.302585092994046, 1e-10, -1e-10,
    };
    for (size_t i = 0; i < sizeof SPECIALS / sizeof SPECIALS[0]; i++) print_unary_input(SPECIALS[i]);
    for (size_t i = 0; i < sizeof UNARY_EDGES / sizeof UNARY_EDGES[0]; i++) {
        for (int d = -4; d <= 4; d++) {
            double x = from_bits(UNARY_EDGES[i] + (uint64_t)(int64_t)d);
            print_unary_input(x);
            print_unary_input(-x);
        }
    }
    for (int n = -1100; n <= 1100; n++) print_unary_input((double)n);
    for (int n = -41; n <= 41; n += 2) print_unary_input((double)n / 2.0);
    for (int e = -1074; e <= 1023; e++) {
        double p = e < -1022 ? from_bits(1ull << (e + 1074)) : from_bits((uint64_t)(e + 1023) << 52);
        print_unary_input(p);
        if (e % 8 == 0) {
            print_unary_input(from_bits(to_bits(p) + 1));
            print_unary_input(from_bits(to_bits(p) - 1));
        }
    }
    double ten = 1.0;
    for (int n = 0; n <= 308; n++, ten *= 10.0) print_unary_input(ten);
    ten = 1.0;
    for (int n = 0; n <= 323; n++, ten /= 10.0) print_unary_input(ten);
    for (int j = 1; j <= 60; j++) {
        double h = from_bits((uint64_t)(1023 - j) << 52); /* 2^-j */
        print_unary_input(1.0 + h);
        print_unary_input(1.0 - h);
        print_unary_input(-1.0 + h);
    }
    for (int k = -1080; k <= 1030; k++) print_unary_input((double)k * 0.6931471805599453);
}

/* Math.sin, cos, tan, asin, acos and atan of a fixed list of doubles, the
 * same lines for any COUNT, each function given every input (8,693 each):
 * specials; the thresholds of fdlibm's ranges (high words, low word 0)
 * and 4 units either side, of either sign: rem_pio2's pi/4, 3pi/4, pi/2
 * and 2^19 pi/2, the kernels' 2^-27, 2^-28, 0.3, 0.78125 and 0.6744, asin's
 * and acos's 1, 0.5, 2^-57 and 0.975, atan's 2^66, 0.4375, 0.6875, 1.1875
 * and 2.4375; every power of two and its neighbours (huge arguments for
 * __kernel_rem_pio2), and integers and halves; the doubles next to k pi/4
 * for k up to 256 and next to k pi/2 for larger k up to 2^20, where
 * argument reduction cancels most; the double closest to a multiple of
 * pi/2 found for each eighth binary exponent and the hardest ones of all
 * (6381956970095103 * 2^797 among them; TRIG_HARD, from the continued
 * fraction of 2^e * 2/pi), with a unit either side; and k/256 in [-1, 1],
 * 1 - 2^-j and 0.5 +- 2^-j, for asin and acos. */
static const uint32_t TRIG_EDGES[] = {
    0x3FE921FB, 0x4002D97C, 0x3FF921FB, 0x413921FB, 0x3E400000, 0x3E300000, 0x3FD33333,
    0x3FE90000, 0x3FE59428, 0x3FF00000, 0x3FE00000, 0x3C600000, 0x3FEF3333, 0x44100000,
    0x3FDC0000, 0x3FF30000, 0x3FE60000, 0x40038000, 0x7FE00000, 0x43300000,
};

static const uint64_t TRIG_HARD[] = {
    0x3FF921FB54442D18ull, 0x4046C6CBC45DC8DEull, 0x4056C6CBC45DC8DEull, 0x4066C6CBC45DC8DEull,
    0x4076C6CBC45DC8DEull, 0x40F65A1DD290660Full, 0x416B951F1572EBA5ull, 0x417B951F1572EBA5ull,
    0x41F5C9508C58AAFAull, 0x427065C829D68730ull, 0x42F7512069B7430Dull, 0x437AE9608C734E12ull,
    0x43FA7F3BC5A7ED9Eull, 0x442782B7A20DF6D4ull, 0x4471A209B98A791Full, 0x44F99CAA5236FEEAull,
    0x457F19E5D71B26BAull, 0x45966BD5424E5655ull, 0x45FC3DC1A597F202ull, 0x467DAE3A00451B5Full,
    0x46F0A68ECA9141AAull, 0x47747623428452F0ull, 0x47F12155B2D3A126ull, 0x482504CAC51F1EAFull,
    0x483504CAC51F1EAFull, 0x487504CAC51F1EAFull, 0x48F5AD5A62CB1CC9ull, 0x497B930D680374C2ull,
    0x49F17E1E60C52F2Eull, 0x4A7CCA7515E2585Cull, 0x4AFD28D5C4C5E73Bull, 0x4B70539B48D14C55ull,
    0x4BFBB4C8E40CBE5Dull, 0x4C4E7E44A78AC18Cull, 0x4C76DEB37DA81129ull, 0x4CF615A7F15B285Dull,
    0x4D7A349D0F75C767ull, 0x4DF33322FD48B212ull, 0x4E72FA9F52E722D3ull, 0x4EF5AF9BF37155EFull,
    0x4F569EAB0985179Bull, 0x4F769EAB0985179Bull, 0x4FCB2196364D750Bull, 0x4FFB2196364D750Bull,
    0x50798D4578B193D2ull, 0x50F16E59932AB3BCull, 0x5177FE9A7DD5DA32ull, 0x51F3E51CBF3B4ECAull,
    0x526C45CD11154DFDull, 0x527C45CD11154DFDull, 0x52F1A73EE3FD4BA9ull, 0x537ED9D1CC08E8A1ull,
    0x53F870A967710516ull, 0x547A93B216E8DFBCull, 0x54F3671F965E06E3ull, 0x557979B4570B7A0Eull,
    0x55FC945B5E25F07Cull, 0x5674582FF44B4254ull, 0x56F64B413DD64FCBull, 0x577005B55082422Eull,
    0x57F715B33A0D143Eull, 0x587CB5C087CE6AB4ull, 0x58F82F9A53010EEFull, 0x597E3CA9B6C655CBull,
    0x59F11BB9A5A2006Dull, 0x5A76DD6B29EDC33Aull, 0x5AF372EC0E7828ECull, 0x5B7A9926BE394007ull,
    0x5BF2E28B9339BD65ull, 0x5C79C487F2337E3Aull, 0x5CFF7AF940F11670ull, 0x5D71888CAC3906F4ull,
    0x5DFFE85261F02860ull, 0x5E7B88CBB4E32576ull, 0x5EF7833FA59D0DFDull, 0x5F760507DE0CF1D1ull,
    0x5FF776AE889D49FCull, 0x607C2F1EEC0AE601ull, 0x60FF32EB85F75857ull, 0x6171FAB41EBE9846ull,
    0x61F3B4801FD74C7Eull, 0x627C9B628EDE4146ull, 0x62F6BF5D7ABAF7C8ull, 0x637D89ED8A1D39A7ull,
    0x63F6F2FF88C4C44Eull, 0x6404C96C11134D36ull, 0x6414C96C11134D36ull, 0x647959CEDBBEE908ull,
    0x64F518ACD4FE45E6ull, 0x65774BF09BBE278Bull, 0x65F5157705A306A2ull, 0x66583009E2E9E2EBull,
    0x66783009E2E9E2EBull, 0x66FE77733CDA9131ull, 0x6775E13DC1C15D0Eull, 0x67F668DB30162B3Dull,
    0x687CB44E86BC192Bull, 0x68FED0721B49942Full, 0x69741C5FCEB2921Dull, 0x69F97E359BB79BB4ull,
    0x6A7DB41F3CB71D7Bull, 0x6AF3ECB76DE9E4BEull, 0x6B729460D8EB8D38ull, 0x6BFF0C9D98414062ull,
    0x6C75AE169CEA43D6ull, 0x6CF87172196D5822ull, 0x6D78C50C5644903Full, 0x6DF21143C12C9DFBull,
    0x6E7BF3EDFA4E619Eull, 0x6EF1B21E01142FB4ull, 0x6F7D5C8D09F26EBFull, 0x6FFAFEB41302D02Aull,
    0x7079E0CF28408E96ull, 0x70F6DE1F1AEFCDBFull, 0x717F3D96FD674993ull, 0x71F52F4FE34C0E00ull,
    0x72700DDA8A3FA29Cull, 0x72FC2AF369704700ull, 0x7370107ED222087Eull, 0x73F6FB0D0AB996FAull,
    0x7471D2340847046Aull, 0x74F9FF0F3CACCDB0ull, 0x7506AC5B262CA1FFull, 0x7516AC5B262CA1FFull,
    0x7526AC5B262CA1FFull, 0x7536AC5B262CA1FFull, 0x7576AC5B262CA1FFull, 0x75BCFE482285F8EDull,
    0x75FCFE482285F8EDull, 0x767A5DE46DB774E4ull, 0x76FE398FE7099B7Aull, 0x777F08B14E1C4D0Full,
    0x77FDD2603AF19862ull, 0x7872FB7AFE08C773ull, 0x78F0332FE835389Aull, 0x797C1755720D0BF3ull,
    0x79FFCE36F3EBFE43ull, 0x7A7A5046CD1FCAFEull, 0x7AF4117573397D42ull, 0x7B7E1987122B7E06ull,
    0x7BFEA17C46D23A3Cull, 0x7C7163DCD0E3814Dull, 0x7CF408E81423EA13ull, 0x7D778875916718DBull,
    0x7DEE009C53148BE1ull, 0x7DFE009C53148BE1ull, 0x7E787B27C09AE521ull, 0x7EF36BF9A50C0440ull,
    0x7F70156ABCADF385ull,
};

/* The double nearest k pi/2, for k < 2^20: k times fdlibm's 33-bit pio2_1
 * is exact, and its tail is added once. */
static double near_pio2(long k) {
    const double pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11;
    return (double)k * pio2_1 + (double)k * pio2_1t;
}

static void print_trig(void) {
    static const double SPECIALS[] = {
        0.0, -0.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 4.9406564584124654e-324, -4.9406564584124654e-324,
        2.2250738585072009e-308, -2.2250738585072009e-308, 2.2250738585072014e-308, -2.2250738585072014e-308,
        1.7976931348623157e308, -1.7976931348623157e308, 1e-300, 1e300, -1e300, 1e22, -1e22, 0.1, -0.1,
        3.141592653589793, -3.141592653589793, 1.5707963267948966, -1.5707963267948966, 0.7853981633974483,
        2.356194490192345, 6.283185307179586, 1e-10, -1e-10, 0.25, 0.75,
    };
    for (size_t i = 0; i < sizeof SPECIALS / sizeof SPECIALS[0]; i++) print_trig_input(SPECIALS[i]);
    for (size_t i = 0; i < sizeof TRIG_EDGES / sizeof TRIG_EDGES[0]; i++) {
        for (int d = -4; d <= 4; d++) {
            double x = from_bits(((uint64_t)TRIG_EDGES[i] << 32) + (uint64_t)(int64_t)d);
            print_trig_input(x);
            print_trig_input(-x);
        }
    }
    for (int e = -1074; e <= 1023; e++) {
        double p = e < -1022 ? from_bits(1ull << (e + 1074)) : from_bits((uint64_t)(e + 1023) << 52);
        print_trig_input(p);
        if (e % 4 == 0) print_trig_input(-p);
        if (e % 8 == 0) {
            print_trig_input(from_bits(to_bits(p) + 1));
            print_trig_input(from_bits(to_bits(p) - 1));
        }
    }
    for (int n = -1000; n <= 1000; n++) print_trig_input((double)n);
    for (int n = -41; n <= 41; n += 2) print_trig_input((double)n / 2.0);
    for (long k = 1; k <= 256; k++) {
        double x = near_pio2(k) * 0.5;
        for (int d = -1; d <= 1; d++) {
            print_trig_input(from_bits(to_bits(x) + (uint64_t)(int64_t)d));
            print_trig_input(-from_bits(to_bits(x) + (uint64_t)(int64_t)d));
        }
    }
    for (long k = 257; k < (1l << 20); k = k * 5 / 4 + 1) {
        double x = near_pio2(k);
        for (int d = -1; d <= 1; d++) {
            print_trig_input(from_bits(to_bits(x) + (uint64_t)(int64_t)d));
            print_trig_input(-from_bits(to_bits(x) + (uint64_t)(int64_t)d));
        }
    }
    for (size_t i = 0; i < sizeof TRIG_HARD / sizeof TRIG_HARD[0]; i++) {
        for (int d = -1; d <= 1; d++) print_trig_input(from_bits(TRIG_HARD[i] + (uint64_t)(int64_t)d));
        print_trig_input(-from_bits(TRIG_HARD[i]));
    }
    for (int n = -256; n <= 256; n++) print_trig_input((double)n / 256.0);
    for (int j = 1; j <= 60; j++) {
        double h = from_bits((uint64_t)(1023 - j) << 52); /* 2^-j */
        print_trig_input(1.0 - h);
        print_trig_input(-1.0 + h);
        print_trig_input(0.5 + h);
        print_trig_input(0.5 - h);
    }
}

/* Math.atan2(y, x) of a fixed list of pairs, the same for any COUNT: every
 * pair of ATAN2_VALUES (zeros, infinities, NaN, +-1, subnormals, the
 * largest and smallest normals: each of atan2's special cases); y / x
 * near 2^60 and 2^-60, where atan2 stops dividing, either sign; x = 1, which
 * calls atan, and x next to 1; and a grid of small integers. */
static const double ATAN2_VALUES[] = {
    0.0, -0.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 1.0, -1.0, 2.0, -2.0, 0.5, -0.5, 3.0, -3.0,
    4.9406564584124654e-324, -4.9406564584124654e-324, 2.2250738585072014e-308, -2.2250738585072014e-308,
    1.7976931348623157e308, -1.7976931348623157e308, 1e-300, -1e300, 0.1, -0.7, 1e10, -1e-10,
};

static void print_atan2(double y, double x) {
    printf("V %016llx %016llx %016llx\n", (unsigned long long)to_bits(y), (unsigned long long)to_bits(x),
           (unsigned long long)to_bits(sjs_math_atan2(y, x)));
}

static void print_atan2s(void) {
    size_t n = sizeof ATAN2_VALUES / sizeof ATAN2_VALUES[0];
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) print_atan2(ATAN2_VALUES[i], ATAN2_VALUES[j]);
    }
    for (int k = -66; k <= 66; k++) {
        double y = 1.5, x = from_bits((uint64_t)(1023 - k) << 52) * 1.25; /* y / x = 1.2 * 2^k */
        print_atan2(y, x);
        print_atan2(-y, x);
        print_atan2(y, -x);
        print_atan2(-y, -x);
        print_atan2(x, from_bits((uint64_t)(1023 + k) << 52)); /* the high words differ by k << 20 */
        print_atan2(-x, -from_bits((uint64_t)(1023 + k) << 52));
    }
    for (size_t i = 0; i < n; i++) {
        print_atan2(ATAN2_VALUES[i], from_bits(0x3FF0000000000001ull));
        print_atan2(ATAN2_VALUES[i], from_bits(0x3FEFFFFFFFFFFFFFull));
    }
    for (int k = -40; k <= 40; k++) print_atan2((double)k / 8.0, 1.0);
    for (int y = -12; y <= 12; y++) {
        for (int x = -12; x <= 12; x++) print_atan2((double)y, (double)x);
    }
}

/* Math.sinh, cosh, tanh, asinh, acosh, atanh and cbrt of a fixed list of
 * doubles, the same lines for any COUNT, each function given every input:
 * specials; the thresholds of fdlibm's ranges and 4 units either side, of
 * either sign: sinh's, cosh's and tanh's 2^-28, 2^-55, 0.5 ln 2, 1 and 22,
 * log(MAX) (0x40862E42, and its exact value) and the overflow threshold
 * 0x408633CE 8FB9F87D; asinh's and acosh's 2^28, 2 and 1, atanh's 0.5 and
 * 1; the smallest normal, where cbrt scales a subnormal; every power of
 * two and its neighbours (each binary exponent mod 3 for cbrt), integers
 * and halves; the doubles from 700 to 712 by 1/16, around the overflow;
 * k/256 in [-1, 1], 1 +- 2^-j and -1 + 2^-j, for atanh and acosh; and the
 * cubes of 1 to 200 with a unit either side, for cbrt. */
static const uint64_t HYPERBOLIC_EDGES[] = {
    0x3E30000000000000ull, 0x3C80000000000000ull, 0x3FD62E4300000000ull, 0x3FD62E42FEFA39EFull,
    0x3FF0000000000000ull, 0x4036000000000000ull, 0x40862E4200000000ull, 0x40862E42FEFA39EFull,
    0x408633CE8FB9F87Dull, 0x41B0000000000000ull, 0x4000000000000000ull, 0x3FE0000000000000ull,
    0x0010000000000000ull,
};

static void print_hyperbolic(void) {
    static const double SPECIALS[] = {
        0.0, -0.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 4.9406564584124654e-324, -4.9406564584124654e-324,
        2.2250738585072009e-308, -2.2250738585072009e-308, 1.7976931348623157e308, -1.7976931348623157e308,
        1e-300, 1e300, -1e300, 0.1, -0.1, 0.25, -0.75, 1e-10, -1e-10, 0.9999999999999999, -0.9999999999999999,
        2.718281828459045, 3.141592653589793, 709.782712893384, 710.4758600739439, 710.475860073944, -710.4758600739439,
    };
    for (size_t i = 0; i < sizeof SPECIALS / sizeof SPECIALS[0]; i++) print_hyperbolic_input(SPECIALS[i]);
    for (size_t i = 0; i < sizeof HYPERBOLIC_EDGES / sizeof HYPERBOLIC_EDGES[0]; i++) {
        for (int d = -4; d <= 4; d++) {
            double x = from_bits(HYPERBOLIC_EDGES[i] + (uint64_t)(int64_t)d);
            print_hyperbolic_input(x);
            print_hyperbolic_input(-x);
        }
    }
    for (int e = -1074; e <= 1023; e++) {
        double p = e < -1022 ? from_bits(1ull << (e + 1074)) : from_bits((uint64_t)(e + 1023) << 52);
        print_hyperbolic_input(p);
        if (e % 4 == 0) print_hyperbolic_input(-p);
        if (e % 8 == 0 || (e >= -6 && e <= 6)) {
            print_hyperbolic_input(from_bits(to_bits(p) + 1));
            print_hyperbolic_input(from_bits(to_bits(p) - 1));
        }
    }
    for (int n = -1000; n <= 1000; n++) print_hyperbolic_input((double)n);
    for (int n = -45; n <= 45; n += 2) print_hyperbolic_input((double)n / 2.0);
    for (int n = 0; n <= 192; n++) print_hyperbolic_input(700.0 + (double)n / 16.0);
    for (int n = -256; n <= 256; n++) print_hyperbolic_input((double)n / 256.0);
    for (int j = 1; j <= 60; j++) {
        double h = from_bits((uint64_t)(1023 - j) << 52); /* 2^-j */
        print_hyperbolic_input(1.0 - h);
        print_hyperbolic_input(-1.0 + h);
        print_hyperbolic_input(1.0 + h);
    }
    for (int k = 1; k <= 200; k++) {
        double c = (double)k * (double)k * (double)k;
        for (int d = -1; d <= 1; d++) print_hyperbolic_input(from_bits(to_bits(c) + (uint64_t)(int64_t)d));
    }
}

static int print(long count) {
    print_formats(count);
    print_decimals(count);
    print_strings(count);
    print_integers(count);
    print_roots(count);
    print_hypots(count);
    print_integer_ops(count);
    print_bigint_ops(count);
    print_bigint_texts(count);
    print_digit_methods();
    print_radix_methods();
    print_powers();
    print_unary();
    print_trig();
    print_atan2s();
    print_hyperbolic();
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

    /* x.toString(radix), as Node 22 writes it. */
    expect_radix(255.0, 16, "ff");
    expect_radix(-255.0, 2, "-11111111");
    expect_radix(0.1, 3, "0.0022002200220022002200220022002201");
    expect_radix(0.1, 36, "0.3lllllllllm");
    expect_radix(0.5, 2, "0.1");
    expect_radix(1.0 / 3.0, 3, "0.1");
    expect_radix(-0.0, 2, "0");
    expect_radix(0.0 / 0.0, 16, "NaN");
    expect_radix(1.0 / 0.0, 36, "Infinity");
    expect_radix(-1.0 / 0.0, 2, "-Infinity");
    expect_radix(1e21, 36, "5v1j4f4ds7c000");
    expect_radix(9007199254740992.0, 7, "5350140446150306054");
    expect_radix(123.456, 10, "123.456");

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
