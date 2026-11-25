/* Unit tests for cli/decimal: a decimal number's text as the nearest double
 * (Go's strconv.ParseFloat, ported). The fixtures are tests/json/numbers/
 * (its README.md says where each comes from): Go's own vectors, hand-picked
 * hard cases, the numbers of the suite's values.txt, and a sample of
 * parse-number-fxx-test-data; then a seeded sample of doubles written with
 * ta_format_double must read back as themselves.
 *
 * --full: the round trips of two million doubles, and the whole
 * parse-number-fxx-test-data corpus when `mise run json:corpus` fetched it
 * into build/parse-number-fxx-test-data. */
#include <stdlib.h>
#include <string.h>

#include "../harness/harness.h"
#include "base/format.h"
#include "cli/decimal.h"

#ifndef TA_JSON_NUMBERS
#define TA_JSON_NUMBERS "tests/json/numbers"
#endif
#ifndef TA_JSON_NUMBERS_CORPUS
#define TA_JSON_NUMBERS_CORPUS "build/parse-number-fxx-test-data"
#endif

static int full_mode;

/* A file's bytes, NUL-terminated (malloc'd), or NULL. */
static char *file_read(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *bytes = (char *)malloc((size_t)size + 1);
    if (bytes && fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);
    if (bytes) bytes[size] = 0;
    *length = (size_t)size;
    return bytes;
}

static uint64_t bits_of(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* The expectation of a line: "invalid", or the double's bits. */
typedef struct expectation {
    int invalid;
    uint64_t bits;
} expectation;

/* `text` read: 1 when it gives what is expected (a failure printed). */
static int reads_as(ta_string_view text, expectation expected, const char *where) {
    ta_decimal_reading reading = {text, 0};
    ta_status status = ta_decimal_read(&reading);
    int same = expected.invalid ? status == TA_ERROR_INVALID
                                : status == TA_OK && bits_of(reading.value) == expected.bits;
    if (!same)
        fprintf(stderr, "  %s: \"%.*s\" gave status %d, bits %016llx; expected %s %016llx\n", where,
                text.length > 80 ? 80 : (int)text.length, text.pointer, (int)status,
                status == TA_OK ? (unsigned long long)bits_of(reading.value) : 0ull,
                expected.invalid ? "invalid" : "bits", (unsigned long long)expected.bits);
    return same;
}

/* A fixture of lines "<text> <bits>" or "<text> invalid" (split at the
 * last space: a text may hold spaces), "#" comments. Returns the cases. */
static uint32_t check_fixture(const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", TA_JSON_NUMBERS, name);
    size_t length;
    char *bytes = file_read(path, &length);
    CHECK(bytes != NULL);
    if (!bytes) return 0;
    uint32_t cases = 0;
    for (char *line = bytes; *line;) {
        char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if (end > line && line[0] != '#') {
            char *space = end - 1;
            while (space > line && *space != ' ')
                space--;
            expectation expected = {0, 0};
            if (end - space - 1 == 7 && strncmp(space + 1, "invalid", 7) == 0) expected.invalid = 1;
            else expected.bits = strtoull(space + 1, NULL, 16);
            CHECK(reads_as((ta_string_view){line, (uint32_t)(space - line)}, expected, name));
            cases++;
        }
        line = *end ? end + 1 : end;
    }
    free(bytes);
    return cases;
}

/* 1 when the text is a JSON number. */
static int is_json_number(ta_string_view text) {
    uint32_t at = 0;
    if (at < text.length && text.pointer[at] == '-') at++;
    if (at == text.length || text.pointer[at] < '0' || text.pointer[at] > '9') return 0;
    if (text.pointer[at++] != '0')
        while (at < text.length && text.pointer[at] >= '0' && text.pointer[at] <= '9')
            at++;
    if (at < text.length && text.pointer[at] == '.') {
        if (++at == text.length || text.pointer[at] < '0' || text.pointer[at] > '9') return 0;
        while (at < text.length && text.pointer[at] >= '0' && text.pointer[at] <= '9')
            at++;
    }
    if (at < text.length && (text.pointer[at] == 'e' || text.pointer[at] == 'E')) {
        at++;
        if (at < text.length && (text.pointer[at] == '+' || text.pointer[at] == '-')) at++;
        if (at == text.length || text.pointer[at] < '0' || text.pointer[at] > '9') return 0;
        while (at < text.length && text.pointer[at] >= '0' && text.pointer[at] <= '9')
            at++;
    }
    return at == text.length;
}

/* A file in parse-number-fxx-test-data's format: "f16 f32 f64 text", the
 * f64 at columns 14 to 30. A text that is not a JSON number must be
 * refused, and so must one whose double is an infinity. Returns the cases. */
static uint32_t check_fxx(const char *path, int must_exist) {
    size_t length;
    char *bytes = file_read(path, &length);
    if (!bytes) {
        if (must_exist) CHECK(bytes != NULL);
        return 0;
    }
    uint32_t cases = 0, failures = 0;
    for (char *line = bytes; *line;) {
        char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if (end - line > 31 && line[0] != '#') {
            ta_string_view text = {line + 31, (uint32_t)(end - line - 31)};
            uint64_t bits = strtoull(line + 14, NULL, 16);
            expectation expected = {!is_json_number(text) || (bits & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull,
                                    bits};
            if (!reads_as(text, expected, path) && ++failures > 20) {
                CHECK(0);
                break;
            }
            cases++;
        }
        line = *end ? end + 1 : end;
    }
    CHECK_EQUAL(failures, 0);
    free(bytes);
    return cases;
}

static void test_go_vectors(void) {
    CHECK(check_fixture("go-atof.txt") >= 80);
}

static void test_hard_cases(void) {
    CHECK(check_fixture("hard.txt") >= 50);
}

static void test_values_numbers(void) {
    CHECK(check_fixture("values.txt") >= 19);
}

static void test_fxx_sample(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/parse-number-fxx-sample.txt", TA_JSON_NUMBERS);
    CHECK(check_fxx(path, 1) > 3000);
}

/* --full: every file of the corpus, when it was fetched. */
static void test_fxx_corpus(void) {
    static const char *const names[] = {
        "freetype-2-7",           "google-double-conversion", "google-wuffs",
        "ibm-fpgen",              "lemire-fast-double-parser", "lemire-fast-float",
        "more-test-cases",        "tencent-rapidjson",         "ulfjack-ryu",
        "remyoudompheng-fptest-0", "remyoudompheng-fptest-1",  "remyoudompheng-fptest-2",
        "remyoudompheng-fptest-3",
    };
    uint32_t cases = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.txt", TA_JSON_NUMBERS_CORPUS, names[i]);
        cases += check_fxx(path, 0);
    }
    if (!cases) fprintf(stderr, "  the corpus is not fetched (mise run json:corpus): skipped\n");
    else fprintf(stderr, "  %u lines of parse-number-fxx-test-data\n", cases);
}

/* xorshift64*: a seeded sequence, the same on every machine. */
static uint64_t next_random(uint64_t *state) {
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return *state * 0x2545F4914F6CDD1Dull;
}

/* Doubles written with ta_format_double read back as themselves: random
 * bit patterns (every exponent), and integers and short decimals. */
static void test_round_trips(void) {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint32_t count = full_mode ? 2000000u : 20000u, failures = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t bits = next_random(&state);
        if (i % 4 == 1) bits &= 0x800FFFFFFFFFFFFFull; /* a subnormal */
        if (i % 4 == 2) { /* an integer, or a decimal of a few digits */
            double value = (double)(next_random(&state) % 1000000) / (double)(1u << (next_random(&state) % 8));
            memcpy(&bits, &value, sizeof bits);
        }
        if ((bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) continue; /* NaN, infinities */
        double value;
        memcpy(&value, &bits, sizeof value);
        char room[64];
        ta_output out = ta_output_over((ta_buffer){room, sizeof room, 0}, NULL);
        ta_format_double(&out, value);
        expectation expected = {0, bits};
        if (!ta_output_fits(&out) ||
            (!reads_as((ta_string_view){room, (uint32_t)out.length}, expected, "round trip") && ++failures > 20))
            break;
    }
    CHECK_EQUAL(failures, 0);
}

/* What ta_decimal_read refuses besides the fixtures': nothing at all, and
 * the text only up to its length (no terminator read). */
static void test_refusals(void) {
    ta_decimal_reading reading = {{"", 0}, 0};
    CHECK_EQUAL(ta_decimal_read(&reading), TA_ERROR_INVALID);
    reading.text = (ta_string_view){"125x", 3};
    CHECK_EQUAL(ta_decimal_read(&reading), TA_OK);
    CHECK(reading.value == 125.0);
    reading.text = (ta_string_view){"1e", 1};
    CHECK_EQUAL(ta_decimal_read(&reading), TA_OK);
    CHECK(reading.value == 1.0);
}

int main(int argc, char **argv) {
    full_mode = argc > 1 && strcmp(argv[1], "--full") == 0;
    RUN(test_go_vectors);
    RUN(test_hard_cases);
    RUN(test_values_numbers);
    RUN(test_fxx_sample);
    RUN(test_round_trips);
    RUN(test_refusals);
    if (full_mode) RUN(test_fxx_corpus);
    TEST_MAIN_END();
}
