/*
 * Numbers without a C library: formatting, remainder and rounding.
 *
 *   number_test                 known cases
 *   number_test --print COUNT   COUNT pseudo-random doubles as "<hex bits> <text>",
 *                               for diff-node to compare with String(x)
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

static int print(long count) {
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
        printf("%016llx %s\n", (unsigned long long)bits, text);
    }
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

    double out;
    if (!sjs_number_from_decimal((sjs_str){ "15", 2 }, -1, &out) || out != 1.5) {
        fprintf(stderr, "from_decimal 15e-1\n");
        failures++;
    }
    if (sjs_number_from_decimal((sjs_str){ "12345678901234567890", 20 }, 0, &out)) {
        fprintf(stderr, "from_decimal took 20 digits\n");
        failures++;
    }

    if (failures != 0) {
        fprintf(stderr, "number_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("number_test: formatting, remainder and rounding agree with JavaScript\n");
    return 0;
}
