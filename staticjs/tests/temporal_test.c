/*
 * Temporal values on fixed lists of inputs (docs/temporal.md): sweeps too
 * large for spec cases, and with faults among them, which one spec case
 * cannot hold.
 *
 *   temporal_test            evaluates every case below: each must compile,
 *                            and nothing may leak
 *   temporal_test --print    lines for diff-node to compare with JavaScript
 *                            (Temporal from tests/vendor/temporal-polyfill),
 *                            each a tag, a space, then fields separated by
 *                            tabs:
 *     Y <source> <outcome> <deliberate>
 *                            a JavaScript expression, as a JSON string; what
 *                            the writer writes for its value, as a JSON
 *                            string, or ! and the fault's message; and 1 when
 *                            staticjs faults on purpose where JavaScript may
 *                            give a value (a part the polyfill reads but the
 *                            specification's grammar, and
 *                            @js-temporal/polyfill 0.5.1, refuses, or a
 *                            calendar other than ISO 8601's), else 0.
 *                            diff-node requires a fault where JavaScript
 *                            throws, JavaScript's value where staticjs gives
 *                            one, and a fault on every deliberate line.
 *
 * The cases, about 44,000, come from fixed lists, without randomness:
 *   - ISO 8601 strings for Temporal.PlainDate.from, Temporal.PlainTime.from
 *     and Temporal.PlainDateTime.from: dates, times, offsets and
 *     annotations, each list varied with the others held;
 *   - times alone for Temporal.PlainTime.from: with and without their T,
 *     clocks of every form, offsets, annotations, and times that read as a
 *     year and month or a month and day;
 *   - dates' getters (day of week and of year, ISO weeks, leap years) over
 *     years at the edges of the calendar's cycles and of Temporal's limits,
 *     on the days around the ends of their years;
 *   - Temporal's limits through the constructor, a bag and a string;
 *   - property bags with every overflow, month, monthCode and day edge, for
 *     from and for with;
 *   - toString's calendarName;
 *   - time fields in bags (from and with of a PlainTime and a
 *     PlainDateTime) and constructors at each field's edges and at
 *     Temporal's limits, toPlainDateTime and withPlainTime;
 *   - round of a PlainTime and a PlainDateTime: every unit, increment and
 *     rounding mode over times at and around each multiple and its halves
 *     and midnight; at the limits; increments that do not divide;
 *   - toString's fractionalSecondDigits and smallestUnit by every rounding
 *     mode, over times at each digit's halves, and at the limits;
 *   - Durations: ISO 8601 durations (signs, date and time parts, each
 *     varied with the others, in both cases) and strings at the limits;
 *     round relative to dates at month ends and leap days over every pair
 *     of units, every mode and increments of the largest unit; round of
 *     24-hour days and time over every unit, increment, mode and largest
 *     unit; total in every unit, with and without relativeTo; toString's
 *     digits by every mode; compare, add and subtract;
 *   - until and since of dates at month ends, leap days and the limits over
 *     every pair of units, mode and some increments; of dates and times on
 *     either side of each other's time; of times over every increment;
 *     add and subtract of durations to those dates by each overflow;
 *   - Instants: ISO 8601 strings (dates, times, offsets, annotations, each
 *     list varied with the others), epochs of every kind of value at and
 *     past the limits and their getters, round over every unit,
 *     increments and mode, toString's digits by every mode and its time
 *     zones, until and since over every pair of units, increment and mode,
 *     add, subtract, compare and equals; the roundings before 1970 that
 *     the polyfills give differently fault, deliberately;
 *   - Date (not Temporal's, compared with Node's own): Date.parse over
 *     strings of the date time string format and beside it (each string
 *     staticjs does not read a deliberate fault), Date.UTC over its
 *     fields' edges, and the getters and texts over times at the edges of
 *     years and the limits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "staticjs.h"

/* ------------------------------------------------------------------------
 * Text
 */

typedef struct {
    char *ptr;
    size_t length;
    size_t capacity;
} text;

static void put_bytes(text *t, const char *bytes, size_t n) {
    if (t->length + n + 1 > t->capacity) {
        size_t capacity = t->capacity < 256 ? 256 : t->capacity;
        while (capacity < t->length + n + 1) capacity *= 2;
        t->ptr = realloc(t->ptr, capacity);
        if (t->ptr == NULL) abort();
        t->capacity = capacity;
    }
    memcpy(t->ptr + t->length, bytes, n);
    t->length += n;
    t->ptr[t->length] = '\0';
}

static void put(text *t, const char *s) {
    put_bytes(t, s, strlen(s));
}

static void putf(text *t, const char *format, double a, double b, double c) {
    char room[128];
    int n = snprintf(room, sizeof room, format, a, b, c);
    put_bytes(t, room, (size_t)n);
}

/* `bytes` as a JSON string, which is also a JavaScript string literal (no
 * U+2028 or U+2029 is in any input). */
static void put_quoted(text *t, const char *bytes, size_t n) {
    put(t, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == '"' || c == '\\') {
            char escape[3] = { '\\', (char)c, '\0' };
            put(t, escape);
        } else if (c < 0x20) {
            char escape[8];
            snprintf(escape, sizeof escape, "\\u%04x", c);
            put(t, escape);
        } else {
            put_bytes(t, (const char *)&c, 1);
        }
    }
    put(t, "\"");
}

/* ------------------------------------------------------------------------
 * Evaluation
 */

static long live = 0;
static int failures = 0;

static void *counted_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    void *p = malloc(size);
    if (p != NULL) live++;
    return p;
}

static void counted_free(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    if (params->ptr != NULL) live--;
    free(params->ptr);
}

static const sjs_allocator_vtable counted_vtable = { counted_alloc, counted_free };

typedef struct {
    sjs_context *context;
    sjs_vm *vm;
    bool print;
    long cases;
} runner;

/* Compile and run `source`; print its line, or check it compiled. */
static void evaluate(runner *r, const text *source, bool deliberate) {
    text outcome = { 0 };
    sjs_program *program;
    sjs_error error;
    sjs_compile_params params = { { r->context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { source->ptr, (uint32_t)source->length }, &error };
    r->cases++;
    if (sjs_compile(&params, &program) != SJS_OK) {
        if (failures++ < 20) fprintf(stderr, "temporal_test: does not compile: %s\n  %.*s\n", source->ptr, (int)error.message.length, error.message.ptr);
        return;
    }
    sjs_run_params run_params = { program, NULL, 0 };
    if (sjs_vm_start(r->vm, &run_params) != SJS_OK) abort();
    sjs_run run = sjs_vm_run(r->vm, UINT64_MAX);
    if (run == SJS_RUN_FAULT) {
        sjs_vm_error(r->vm, &error);
        put(&outcome, "!");
        put_bytes(&outcome, error.message.ptr, error.message.length);
    } else {
        sjs_value result;
        if (run != SJS_RUN_DONE || sjs_vm_take_result(r->vm, &result) != SJS_OK) abort();
        sjs_writer *writer;
        sjs_writer_config config = { r->context, result, { "<function>", 10 } };
        if (sjs_writer_init(&writer, &config) != SJS_OK) abort();
        uint8_t room[4096];
        sjs_buffer buffer = { room, sizeof room, 0 };
        if (sjs_writer_write(writer, &buffer) != SJS_OK) abort();
        sjs_writer_destroy(writer);
        text written = { 0 };
        put_bytes(&written, (const char *)room, buffer.length);
        put_quoted(&outcome, written.ptr, written.length);
        free(written.ptr);
        sjs_release(r->context, result);
    }
    sjs_program_release(program);
    if (r->print) {
        text line = { 0 };
        put(&line, "Y ");
        put_quoted(&line, source->ptr, source->length);
        put(&line, "\t");
        put_bytes(&line, outcome.ptr, outcome.length);
        put(&line, deliberate ? "\t1" : "\t0");
        puts(line.ptr);
        free(line.ptr);
    } else if (deliberate && outcome.ptr[0] != '!') {
        if (failures++ < 20) fprintf(stderr, "temporal_test: a deliberate fault gives a value: %s\n", source->ptr);
    }
    free(outcome.ptr);
}

/* ------------------------------------------------------------------------
 * ISO 8601 strings
 */

/* A part of a string, and whether staticjs refuses it on purpose where the
 * polyfill reads it (`refused`): what the specification's grammar, and
 * @js-temporal/polyfill 0.5.1, refuse, and a calendar other than ISO 8601's. */
typedef struct {
    const char *text;
    bool refused;
} part;

static const part DATES[] = {
    { "2024-01-31", 0 }, { "20240131", 0 }, { "+002024-01-31", 0 }, { "+0020240131", 0 }, { "-002024-01-31", 0 },
    { "-0020240131", 0 }, { "-000000-01-01", 0 }, { "+000000-01-01", 0 }, { "2024-02-29", 0 }, { "2023-02-29", 0 },
    { "2024-02-30", 0 }, { "2024-13-01", 0 }, { "2024-00-01", 0 }, { "2024-01-00", 0 }, { "2024-01-32", 0 },
    { "2024-04-31", 0 }, { "1900-02-29", 0 }, { "2000-02-29", 0 }, { "-271821-04-19", 0 }, { "-271821-04-18", 0 },
    { "+275760-09-13", 0 }, { "+275760-09-14", 0 }, { "-271822-01-01", 0 }, { "+275761-01-01", 0 }, { "2024-0131", 0 },
    { "202401-31", 0 }, { "024-01-31", 0 }, { "12024-01-31", 0 }, { "+12024-01-31", 0 }, { "+2024-01-31", 0 },
    { "2024-1-31", 0 }, { "2024-01-1", 0 }, { "0000-01-01", 0 }, { "9999-12-31", 0 }, { "+010000-01-01", 0 },
    { "-000001-12-31", 0 }, { "\xe2\x88\x92" "002024-01-31", 0 }, { "2024/01/31", 0 }, { "2024-01-31 ", 0 },
    { " 2024-01-31", 0 }, { "", 0 }, { "2024", 0 }, { "2024-01", 0 }, { "--01-31", 0 }, { "2024-W05-3", 0 },
    { "2024-031", 0 }, { "x2024-01-31", 0 }, { "2024-01-31x", 0 },
};

static const part TIMES[] = {
    { "", 0 }, { "T00:00", 0 }, { "T10", 0 }, { "T1000", 0 }, { "T10:00", 0 }, { "T10:00:00", 0 }, { "T100000", 0 },
    { "T10:00:00.1", 0 }, { "T10:00:00,123456789", 0 }, { "T10:00:00.1234567890", 0 }, { "t10:00", 0 }, { " 10:00", 0 },
    { "T24:00", 0 }, { "T23:59:60", 0 }, { "T23:60", 0 }, { "T10:0000", 0 }, { "T1000:00", 0 }, { "T10:00.5", 0 },
    { "T", 0 }, { "T1", 0 }, { "T10:00:00.", 0 }, { "T00:00:00.000000001", 0 }, { "T23:59:59.999999999", 0 },
    { "T10:00:00.000", 0 }, { "T10:00:60.5", 0 }, { "T-1", 0 }, { "T10:00:00Z", 0 }, { "  10:00", 0 },
    { "T10:00:00.123,4", 0 },
};

static const part OFFSETS[] = {
    { "", 0 }, { "Z", 0 }, { "z", 0 }, { "+01:00", 0 }, { "-01:00", 0 }, { "+0100", 0 }, { "+01", 0 },
    { "+01:00:00", 0 }, { "+01:00:00.5", 0 }, { "+010000", 0 }, { "+010000.5", 0 }, { "+24:00", 1 },
    { "+23:59", 0 }, { "+23:60", 1 }, { "+01:0", 0 }, { "+1:00", 0 }, { "\xe2\x88\x92" "01:00", 0 }, { "+01:00:60", 1 },
    { "+01:0000", 0 }, { "+0100:00", 0 }, { "-00:00", 0 }, { "+00:00:00.000000000", 0 }, { "+01:00:00.1234567891", 0 },
    { "+", 0 }, { "Z[UTC]", 0 },
};

static const part ANNOTATIONS[] = {
    { "", 0 }, { "[UTC]", 0 }, { "[Europe/Paris]", 0 }, { "[!Europe/Paris]", 0 }, { "[Foo/Bar]", 0 }, { "[foo]", 0 },
    { "[+01:00]", 0 }, { "[+0100]", 0 }, { "[+01]", 0 }, { "[+01:00:00]", 1 }, { "[-23:59]", 0 }, { "[+24:00]", 1 },
    { "[u-ca=iso8601]", 0 }, { "[!u-ca=iso8601]", 0 }, { "[u-ca=ISO8601]", 0 }, { "[u-ca=Iso8601]", 0 },
    { "[u-ca=gregory]", 1 }, { "[!u-ca=gregory]", 1 }, { "[u-ca=]", 1 }, { "[u-ca=iso8601][u-ca=gregory]", 0 },
    { "[!u-ca=iso8601][u-ca=iso8601]", 0 }, { "[u-ca=iso8601][!u-ca=iso8601]", 0 }, { "[u-ca=gregory][u-ca=iso8601]", 1 },
    { "[foo=bar]", 0 }, { "[!foo=bar]", 0 }, { "[Foo=bar]", 0 }, { "[fOo=bar]", 0 }, { "[_foo=bar-baz]", 0 },
    { "[foo=bar--baz]", 1 }, { "[foo=-bar]", 1 }, { "[foo=bar-]", 1 }, { "[foo=b_r]", 1 }, { "[f-o_0=b4r]", 0 },
    { "[0foo=bar]", 1 }, { "[-foo=bar]", 1 }, { "[foo=bar=baz]", 1 }, { "[UTC][u-ca=iso8601]", 0 },
    { "[u-ca=iso8601][UTC]", 1 }, { "[UTC][UTC]", 0 }, { "[UTC][!foo=bar]", 0 }, { "[]", 1 }, { "[!]", 1 }, { "[=]", 1 },
    { "[foo=]", 1 }, { "[=bar]", 1 },
    { "[foo=bar][foo=bar][foo=bar][foo=bar][foo=bar][foo=bar][foo=bar][foo=bar][foo=bar]", 0 },
    { "[foo=bar][foo=bar][foo=bar][foo=bar][foo=bar][foo=bar][foo=bar][foo=bar][foo=bar][foo=bar]", 0 },
    { "[UTC][a=b][a=b][a=b][a=b][a=b][a=b][a=b][a=b]", 0 }, { "[UTC][a=b][a=b][a=b][a=b][a=b][a=b][a=b][a=b][a=b]", 0 },
    { "[.]", 0 }, { "[..]", 0 }, { "[a/./b]", 0 }, { "[a/../b]", 0 }, { "[Etc/GMT+5]", 0 }, { "[Etc/GMT-14]", 0 },
    { "[a//b]", 1 }, { "[/a]", 1 }, { "[a/]", 1 }, { "[1abc]", 1 }, { "[_abc]", 0 }, { "[.abc]", 0 }, { "[-abc]", 1 },
    { "[a+b]", 0 }, { "[a b]", 1 }, { "[u-ca=iso8601", 0 }, { "[u-ca=iso8601]x", 0 }, { "[u-ca=iso8601]]", 0 },
    { "[[u-ca=iso8601]]", 0 }, { "[UTC]x", 0 }, { "[\xc3\xbc]", 1 }, { "[u-ca=iso8601 ]", 0 }, { "[ u-ca=iso8601]", 1 },
    { "[u-ca=iso 8601]", 0 }, { "[u-ca=japanese]", 0 }, { "[!u-ca=ISO8601]", 0 }, { "[u-ca=iso8601-x]", 0 },
    { "[!UTC][!u-ca=iso8601]", 0 },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* `function` (Temporal.PlainDate.from, ...) of the parts joined; each
 * string once. */
typedef struct {
    text seen;              /* every string so far, each followed by a NUL */
    const char *function;
} strings;

static bool seen_before(strings *s, const text *string) {
    for (size_t at = 0; at < s->seen.length; at += strlen(s->seen.ptr + at) + 1) {
        if (strcmp(s->seen.ptr + at, string->ptr) == 0) return true;
    }
    put_bytes(&s->seen, string->ptr, string->length + 1);
    return false;
}

/* Dates in the grammar's form that are no day of the calendar.
 * temporal-polyfill reads the time after one, @js-temporal/polyfill
 * refuses it: staticjs refuses it on purpose. */
static const char *const INVALID_DATES[] = { "2023-02-29", "2024-02-30", "2024-13-01", "2024-00-01", "2024-01-00", "2024-01-32", "2024-04-31", "1900-02-29" };

static bool invalid_date(const char *date) {
    for (size_t i = 0; i < sizeof INVALID_DATES / sizeof INVALID_DATES[0]; i++) {
        if (strcmp(date, INVALID_DATES[i]) == 0) return true;
    }
    return false;
}

static void from_string(runner *r, strings *s, const part *parts[4]) {
    text string = { 0 };
    put(&string, "");
    bool refused = strcmp(s->function, "Temporal.PlainTime.from") == 0 && invalid_date(parts[0]->text) && parts[1]->text[0] != '\0';
    for (int i = 0; i < 4; i++) {
        put(&string, parts[i]->text);
        refused = refused || parts[i]->refused;
    }
    if (!seen_before(s, &string)) {
        text source = { 0 };
        put(&source, s->function);
        put(&source, "(");
        put_quoted(&source, string.ptr, string.length);
        put(&source, ")");
        evaluate(r, &source, refused);
        free(source.ptr);
    }
    free(string.ptr);
}

static void iso_strings(runner *r, const char *function) {
    static const part none = { "", 0 }, at_ten = { "T10:00", 0 }, plus_one = { "+01:00", 0 }, zulu = { "Z", 0 }, utc = { "[UTC]", 0 };
    static const part edges[] = { { "2024-01-31", 0 }, { "-271821-04-19", 0 }, { "+275760-09-13", 0 } };
    static const part offset_times[] = { { "T10:00", 0 }, { "T10", 0 }, { "T1000", 0 }, { "T10:00:00.5", 0 } };
    static const part bases[] = { { "2024-01-31", 0 }, { "2024-01-31T10:00", 0 }, { "2024-01-31T10:00+01:00", 0 }, { "20240131T1000-0100", 0 } };
    strings s = { { 0 }, function };
    for (size_t i = 0; i < COUNT(DATES); i++) {
        from_string(r, &s, (const part *[4]){ &DATES[i], &none, &none, &none });
        from_string(r, &s, (const part *[4]){ &DATES[i], &at_ten, &none, &none });
    }
    for (size_t i = 0; i < COUNT(edges); i++) {
        for (size_t j = 0; j < COUNT(TIMES); j++) {
            from_string(r, &s, (const part *[4]){ &edges[i], &TIMES[j], &none, &none });
            from_string(r, &s, (const part *[4]){ &edges[i], &TIMES[j], &plus_one, &none });
            from_string(r, &s, (const part *[4]){ &edges[i], &TIMES[j], &zulu, &none });
        }
    }
    for (size_t i = 0; i < COUNT(offset_times); i++) {
        for (size_t j = 0; j < COUNT(OFFSETS); j++) {
            from_string(r, &s, (const part *[4]){ &DATES[0], &offset_times[i], &OFFSETS[j], &none });
            from_string(r, &s, (const part *[4]){ &DATES[0], &offset_times[i], &OFFSETS[j], &utc });
        }
    }
    for (size_t i = 0; i < COUNT(bases); i++) {
        for (size_t j = 0; j < COUNT(ANNOTATIONS); j++) from_string(r, &s, (const part *[4]){ &bases[i], &none, &none, &ANNOTATIONS[j] });
    }
    free(s.seen.ptr);
}

/* ------------------------------------------------------------------------
 * Getters, limits, bags
 */

/* Every getter of dates around the ends of years at the edges of the
 * calendar's cycles, and of Temporal's limits. */
static void getters(runner *r) {
    static const double years[] = { -271821, -271820, -100000, -2001, -401, -400, -101, -100, -5, -4, -1, 0, 1, 4, 99, 100,
                                    1582, 1600, 1899, 1900, 1969, 1970, 1971, 1999, 2000, 2001, 2004, 2015, 2020, 2021,
                                    2022, 2023, 2024, 2025, 2026, 2027, 2028, 2100, 2400, 9999, 10000, 100000, 275759, 275760 };
    static const double days[][2] = { { 1, 1 }, { 1, 2 }, { 1, 3 }, { 1, 4 }, { 1, 5 }, { 1, 6 }, { 1, 7 }, { 1, 8 },
                                      { 2, 28 }, { 2, 29 }, { 3, 1 }, { 4, 19 }, { 6, 15 }, { 9, 13 }, { 12, 24 },
                                      { 12, 25 }, { 12, 26 }, { 12, 27 }, { 12, 28 }, { 12, 29 }, { 12, 30 }, { 12, 31 } };
    for (size_t i = 0; i < COUNT(years); i++) {
        for (size_t j = 0; j < COUNT(days); j++) {
            text source = { 0 };
            put(&source, "(d => [d.year, d.month, d.monthCode, d.day, d.dayOfWeek, d.dayOfYear, d.weekOfYear, d.yearOfWeek, "
                         "d.daysInWeek, d.daysInMonth, d.daysInYear, d.monthsInYear, d.inLeapYear, d.toString()])");
            putf(&source, "(new Temporal.PlainDate(%.17g, %.17g, %.17g))", years[i], days[j][0], days[j][1]);
            evaluate(r, &source, false);
            free(source.ptr);
        }
    }
}

/* Temporal's limits, through the constructor, a bag and a string. */
static void limits(runner *r) {
    static const double dates[][3] = { { -271821, 4, 18 }, { -271821, 4, 19 }, { -271821, 4, 20 }, { -271821, 1, 1 },
                                       { -271822, 12, 31 }, { 275760, 9, 12 }, { 275760, 9, 13 }, { 275760, 9, 14 },
                                       { 275760, 12, 31 }, { 275761, 1, 1 }, { 1e9, 1, 1 }, { -1e9, 1, 1 } };
    static const char *const forms[] = {
        "new Temporal.PlainDate(%.17g, %.17g, %.17g)",
        "Temporal.PlainDate.from({ year: %.17g, month: %.17g, day: %.17g })",
        "Temporal.PlainDate.from({ year: %.17g, month: %.17g, day: %.17g }, { overflow: \"reject\" })",
        "new Temporal.PlainDate(2000, 1, 1).with({ year: %.17g, month: %.17g, day: %.17g })",
        "Temporal.PlainDate.compare(new Temporal.PlainDate(%.17g, %.17g, %.17g), \"2000-01-01\")",
    };
    for (size_t i = 0; i < COUNT(dates); i++) {
        for (size_t j = 0; j < COUNT(forms); j++) {
            text source = { 0 };
            putf(&source, forms[j], dates[i][0], dates[i][1], dates[i][2]);
            evaluate(r, &source, false);
            free(source.ptr);
        }
    }
}

/* from and with over every overflow, with months, monthCodes and days at
 * their edges. */
static void bags(runner *r) {
    static const char *const overflows[] = { "", ", { overflow: \"constrain\" }", ", { overflow: \"reject\" }" };
    static const double years[] = { 2023, 2024, -271821, 275760 };
    static const double months[] = { 1, 2, 4, 9, 12, 13, 0, -1, 1.5, 0.5, 1e9 };
    static const double days[] = { 0, 1, 18, 19, 28, 29, 30, 31, 32, 0.5, 1e9 };
    static const char *const codes[] = { "M01", "M02", "M09", "M12", "M13", "M00", "M1", "M01L", "m01", "M001" };
    for (size_t o = 0; o < COUNT(overflows); o++) {
        for (size_t y = 0; y < COUNT(years); y++) {
            for (size_t m = 0; m < COUNT(months); m++) {
                for (size_t d = 0; d < COUNT(days); d++) {
                    text source = { 0 };
                    putf(&source, "Temporal.PlainDate.from({ year: %.17g, month: %.17g, day: %.17g }", years[y], months[m], days[d]);
                    put(&source, overflows[o]);
                    put(&source, ")");
                    evaluate(r, &source, false);
                    free(source.ptr);
                }
            }
        }
        for (size_t c = 0; c < COUNT(codes); c++) {
            for (size_t d = 0; d < COUNT(days); d++) {
                text source = { 0 };
                put(&source, "Temporal.PlainDate.from({ year: 2024, monthCode: \"");
                put(&source, codes[c]);
                putf(&source, "\", day: %.17g }", days[d], 0, 0);
                put(&source, overflows[o]);
                put(&source, ")");
                evaluate(r, &source, false);
                free(source.ptr);
                /* With a month too, which must agree. */
                source = (text){ 0 };
                put(&source, "Temporal.PlainDate.from({ year: 2024, month: 2, monthCode: \"");
                put(&source, codes[c]);
                putf(&source, "\", day: %.17g }", days[d], 0, 0);
                put(&source, overflows[o]);
                put(&source, ")");
                evaluate(r, &source, false);
                free(source.ptr);
            }
        }
        static const char *const changes[] = {
            "{ day: 1 }", "{ day: 29 }", "{ day: 31 }", "{ day: 32 }", "{ day: 0 }", "{ month: 2 }", "{ month: 4 }",
            "{ month: 13 }", "{ month: 0 }", "{ monthCode: \"M02\" }", "{ monthCode: \"M13\" }", "{ year: 2023 }",
            "{ year: 2024 }", "{ year: 275760 }", "{ year: -271821 }", "{ month: 2, monthCode: \"M02\" }",
            "{ month: 3, monthCode: \"M02\" }", "{ year: 2023, month: 2, day: 29 }", "{ month: 2, day: 30 }",
            "{ monthCode: \"M04\", day: 31 }", "{}", "{ era: \"ce\" }", "{ hour: 1 }", "{ calendar: \"iso8601\" }",
            "{ timeZone: \"UTC\" }", "{ day: undefined }", "{ day: null }", "{ day: true }", "{ year: -0.5 }",
        };
        static const char *const receivers[] = { "2024-01-31", "2024-02-29", "2023-03-31", "+275760-09-13", "-271821-04-19" };
        for (size_t v = 0; v < COUNT(receivers); v++) {
            for (size_t c = 0; c < COUNT(changes); c++) {
                text source = { 0 };
                put(&source, "Temporal.PlainDate.from(\"");
                put(&source, receivers[v]);
                put(&source, "\").with(");
                put(&source, changes[c]);
                put(&source, overflows[o]);
                put(&source, ")");
                evaluate(r, &source, false);
                free(source.ptr);
            }
        }
    }
}

/* toString's calendarName, and what it leaves out or adds. */
static void calendar_names(runner *r) {
    static const char *const options[] = { "", "{}", "{ calendarName: \"auto\" }", "{ calendarName: \"never\" }",
                                           "{ calendarName: \"always\" }", "{ calendarName: \"critical\" }",
                                           "{ calendarName: \"Always\" }", "{ calendarName: \"x\" }",
                                           "{ calendarName: undefined }", "{ calendarName: null }", "[]" };
    static const char *const dates[] = { "2024-01-31", "-000001-12-31", "+010000-01-01", "0000-01-01", "9999-12-31" };
    for (size_t d = 0; d < COUNT(dates); d++) {
        for (size_t o = 0; o < COUNT(options); o++) {
            text source = { 0 };
            put(&source, "Temporal.PlainDate.from(\"");
            put(&source, dates[d]);
            put(&source, "\").toString(");
            put(&source, options[o]);
            put(&source, ")");
            evaluate(r, &source, false);
            free(source.ptr);
        }
    }
}

/* ------------------------------------------------------------------------
 * PlainTime and PlainDateTime
 */

/* `source` with `a` in place of %s, evaluated. */
static void evaluate_with(runner *r, const char *format, const char *a, bool deliberate) {
    text source = { 0 };
    const char *at = strstr(format, "%s");
    put_bytes(&source, format, (size_t)(at - format));
    put(&source, a);
    put(&source, at + 2);
    evaluate(r, &source, deliberate);
    free(source.ptr);
}

/* Times alone, as Temporal.PlainTime.from reads them: with and without
 * their T, clocks of every form, offsets and annotations; then times that
 * read as a year and month or a month and day, which need their T. */
static void time_strings(runner *r) {
    static const char *const prefixes[] = { "", "T", "t" };
    static const char *const clocks[] = {
        "12", "1230", "12:30", "123045", "12:30:45", "12:30:45.5", "12:30:45,123456789", "123045.000000001",
        "12:30:45.1234567890", "23:59:60", "23:59:60.999", "24:00", "00:00", "1", "123", "12:3", "12:30:4",
        "12:30.5", "12.5", "1230.5", "12:3045", "1230:45", "", "12:30:45.", "-12", "12 30", "\xef\xbc\x91\xef\xbc\x92",
    };
    static const part offsets[] = { { "", 0 }, { "+01:00", 0 }, { "-05", 0 }, { "+0530", 0 }, { "Z", 0 }, { "z", 0 }, { "-24:00", 0 },
                                    { "+23:59:59.999999999", 0 }, { "+01:00:60", 1 }, { "+1", 0 } };
    static const part annotations[] = {
        { "", 0 }, { "[UTC]", 0 }, { "[u-ca=iso8601]", 0 }, { "[!u-ca=iso8601]", 0 }, { "[u-ca=gregory]", 1 },
        { "[!u-ca=gregory]", 1 }, { "[foo=bar]", 0 }, { "[!foo=bar]", 0 }, { "[+01:00]", 0 }, { "[UTC][u-ca=iso8601]", 0 },
        { "[u-ca=iso8601][u-ca=gregory]", 0 }, { "[u-ca=gregory][u-ca=iso8601]", 1 }, { "[UTC][UTC]", 0 }, { "[u-ca=iso8601", 0 },
    };
    for (size_t p = 0; p < COUNT(prefixes); p++) {
        for (size_t c = 0; c < COUNT(clocks); c++) {
            for (size_t o = 0; o < COUNT(offsets); o++) {
                text string = { 0 };
                put(&string, prefixes[p]);
                put(&string, clocks[c]);
                put(&string, offsets[o].text);
                text source = { 0 };
                put(&source, "Temporal.PlainTime.from(");
                put_quoted(&source, string.ptr, string.length);
                put(&source, ")");
                evaluate(r, &source, offsets[o].refused);
                free(source.ptr);
                free(string.ptr);
            }
        }
        static const char *const annotated[] = { "12:30", "1230-01", "12:30:45.5+01:00" };
        for (size_t c = 0; c < COUNT(annotated); c++) {
            for (size_t a = 0; a < COUNT(annotations); a++) {
                text source = { 0 };
                put(&source, "Temporal.PlainTime.from(\"");
                put(&source, prefixes[p]);
                put(&source, annotated[c]);
                put(&source, annotations[a].text);
                put(&source, "\")");
                evaluate(r, &source, annotations[a].refused);
                free(source.ptr);
            }
        }
    }
    /* Month and day, and year and month, against hours and minutes. */
    static const char *const firsts[] = { "00", "01", "02", "04", "09", "11", "12", "13", "19", "20", "23", "24" };
    static const char *const seconds[] = { "00", "01", "28", "29", "30", "31", "32", "59", "60" };
    static const char *const forms[] = { "%s%s", "%s-%s", "T%s%s", "%s%s[UTC]", "%s%s+01", "%s-%s[u-ca=iso8601]", "%s%s00", "%s%s-01" };
    for (size_t a = 0; a < COUNT(firsts); a++) {
        for (size_t b = 0; b < COUNT(seconds); b++) {
            for (size_t f = 0; f < COUNT(forms); f++) {
                char string[64];
                snprintf(string, sizeof string, forms[f], firsts[a], seconds[b]);
                evaluate_with(r, "Temporal.PlainTime.from(\"%s\")", string, false);
            }
        }
    }
    static const char *const years[] = { "0000", "0012", "1214", "2012", "2021", "2359", "2400", "9999" };
    static const char *const months[] = { "00", "01", "12", "13", "59" };
    static const char *const year_forms[] = { "%s%s", "%s-%s", "T%s%s", "%s%s[u-ca=iso8601]", "%s-%s[UTC]", "%s%s-01" };
    for (size_t y = 0; y < COUNT(years); y++) {
        for (size_t m = 0; m < COUNT(months); m++) {
            for (size_t f = 0; f < COUNT(year_forms); f++) {
                char string[64];
                snprintf(string, sizeof string, year_forms[f], years[y], months[m]);
                evaluate_with(r, "Temporal.PlainTime.from(\"%s\")", string, false);
            }
        }
    }
}

/* Bags of time fields, for PlainTime.from, a PlainTime's with and
 * PlainDateTime.from, over every overflow and each field's edges. */
static void time_bags(runner *r) {
    static const char *const overflows[] = { "", ", { overflow: \"constrain\" }", ", { overflow: \"reject\" }" };
    static const char *const fields[] = { "hour", "minute", "second", "millisecond", "microsecond", "nanosecond" };
    static const char *const values[] = { "-1", "0", "1", "23", "24", "59", "60", "999", "1000", "1e9", "-0.5", "0.5", "-1e9", "null", "true" };
    static const char *const forms[] = {
        "Temporal.PlainTime.from({ %s })",
        "Temporal.PlainTime.from(\"12:34:56.789\").with({ %s })",
        "Temporal.PlainDateTime.from({ year: 2024, month: 2, day: 29, %s })",
        "Temporal.PlainDateTime.from(\"2024-02-29T12:34:56.789\").with({ %s })",
        "Temporal.PlainDateTime.from({ year: 275760, month: 9, day: 13, %s })",
        "Temporal.PlainDateTime.from({ year: -271821, month: 4, day: 19, %s })",
    };
    for (size_t o = 0; o < COUNT(overflows); o++) {
        for (size_t f = 0; f < COUNT(forms); f++) {
            for (size_t i = 0; i < COUNT(fields); i++) {
                for (size_t v = 0; v < COUNT(values); v++) {
                    char bag[64];
                    snprintf(bag, sizeof bag, "%s: %s", fields[i], values[v]);
                    text source = { 0 };
                    const char *at = strstr(forms[f], "%s");
                    put_bytes(&source, forms[f], (size_t)(at - forms[f]));
                    put(&source, bag);
                    put(&source, at + 2);
                    source.length--;   /* the closing parenthesis, after the options */
                    source.ptr[source.length] = '\0';
                    put(&source, overflows[o]);
                    put(&source, ")");
                    evaluate(r, &source, false);
                    free(source.ptr);
                }
            }
        }
    }
}

/* The constructors at each field's edges, and PlainDateTime's at the
 * limits. */
static void constructors(runner *r) {
    static const char *const values[] = { "-1", "0", "1", "23", "24", "59", "60", "999", "1000", "-0.5", "0.5", "undefined", "NaN", "Infinity", "null", "false" };
    for (int position = 0; position < 6; position++) {
        for (size_t v = 0; v < COUNT(values); v++) {
            text args = { 0 };
            for (int i = 0; i < 6; i++) {
                if (i > 0) put(&args, ", ");
                put(&args, i == position ? values[v] : "1");
            }
            evaluate_with(r, "new Temporal.PlainTime(%s)", args.ptr, false);
            evaluate_with(r, "new Temporal.PlainDateTime(2024, 1, 31, %s)", args.ptr, false);
            free(args.ptr);
        }
    }
    static const char *const edges[] = {
        "-271821, 4, 19", "-271821, 4, 19, 0, 0, 0, 0, 0, 1", "-271821, 4, 19, 0, 0, 0, 0, 0, 0", "-271821, 4, 18, 23, 59, 59, 999, 999, 999",
        "-271821, 4, 20", "275760, 9, 13", "275760, 9, 13, 23, 59, 59, 999, 999, 999", "275760, 9, 14", "275760, 9, 14, 0, 0, 0, 0, 0, 0",
        "-271822, 12, 31, 23", "275761, 1, 1", "2024, 2, 29, 23, 59, 59, 999, 999, 999", "2023, 2, 29, 12",
    };
    for (size_t e = 0; e < COUNT(edges); e++) {
        evaluate_with(r, "new Temporal.PlainDateTime(%s)", edges[e], false);
        evaluate_with(r, "(d => [d.toString(), d.toPlainDate(), d.toPlainTime(), d.dayOfWeek, d.weekOfYear])(new Temporal.PlainDateTime(%s))", edges[e], false);
        evaluate_with(r, "Temporal.PlainDateTime.compare(new Temporal.PlainDateTime(%s), \"2000-01-01T12:00\")", edges[e], false);
    }
    static const char *const dates[] = { "new Temporal.PlainDate(-271821, 4, 19)", "new Temporal.PlainDate(275760, 9, 13)", "new Temporal.PlainDate(2024, 1, 31)" };
    static const char *const times[] = { "", "\"00:00\"", "\"00:00:00.000000001\"", "\"23:59:59.999999999\"", "{ hour: 99 }", "{ hour: 99 }, { overflow: \"reject\" }" };
    for (size_t d = 0; d < COUNT(dates); d++) {
        for (size_t t = 0; t < COUNT(times); t++) {
            text source = { 0 };
            put(&source, dates[d]);
            put(&source, ".toPlainDateTime(");
            put(&source, times[t]);
            put(&source, ")");
            evaluate(r, &source, false);
            free(source.ptr);
            source = (text){ 0 };
            put(&source, "Temporal.PlainDateTime.from(");
            put(&source, dates[d]);
            put(&source, ").withPlainTime(");
            put(&source, times[t]);
            put(&source, ")");
            evaluate(r, &source, false);
            free(source.ptr);
        }
    }
}

/* Rounding: every unit a PlainTime and a PlainDateTime round to, every
 * increment that divides the next unit, every mode, over times at and
 * around each multiple, its halves and midnight. */
static const char *const MODES[] = { "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven" };
static const char *const UNIT_NAMES[] = { "nanosecond", "microsecond", "millisecond", "second", "minute", "hour", "day" };
static const long long UNIT_NS[] = { 1, 1000, 1000000, 1000000000, 60000000000, 3600000000000, 86400000000000 };
#define DAY_NS 86400000000000LL

/* A time of day, as a constructor's arguments after `date` (a
 * PlainDateTime's) or alone. */
static void put_time_arguments(text *t, long long ns) {
    char room[96];
    snprintf(room, sizeof room, "%lld, %lld, %lld, %lld, %lld, %lld", ns / 3600000000000LL, ns / 60000000000LL % 60,
             ns / 1000000000LL % 60, ns / 1000000LL % 1000, ns / 1000LL % 1000, ns % 1000);
    put(t, room);
}

/* The times to round to a quantum `q`. `late_tie` leaves out the tie
 * before midnight. */
typedef struct {
    long long q;
    bool late_tie;
} quantum;

static size_t times_for(quantum quantum, long long *out) {
    long long q = quantum.q;
    long long half = q / 2;
    long long candidates[] = { 0, 1, half - 1, half, half + 1, q - 1, q, q + half, 2 * q - 1, quantum.late_tie ? DAY_NS - half : 0, DAY_NS - 1,
                               49651123456789LL };
    size_t n = 0;
    for (size_t i = 0; i < COUNT(candidates); i++) {
        long long c = candidates[i];
        bool seen = false;
        for (size_t j = 0; j < n; j++) seen = seen || out[j] == c;
        if (c >= 0 && c < DAY_NS && !seen) out[n++] = c;
    }
    return n;
}

static void put_times(text *t, bool dated, quantum q) {
    long long times[16];
    size_t n = times_for(q, times);
    put(t, "[");
    for (size_t i = 0; i < n; i++) {
        put(t, i > 0 ? ", " : "");
        put(t, dated ? "new Temporal.PlainDateTime(2024, 2, 29, " : "new Temporal.PlainTime(");
        put_time_arguments(t, times[i]);
        put(t, ")");
    }
    put(t, "]");
}

static void rounding_sweep(runner *r) {
    for (int dated = 0; dated < 2; dated++) {
        for (int unit = 0; unit < (dated ? 7 : 6); unit++) {
            long long parts = unit == 6 ? 2 : unit == 5 ? 24 : unit >= 3 ? 60 : 1000;
            for (long long increment = 1; increment < parts; increment++) {
                if (parts % increment != 0) continue;
                for (size_t m = 0; m < COUNT(MODES); m++) {
                    /* halfEven on a tie: temporal-polyfill makes the
                     * multiple even counted from midnight, the
                     * specification (and @js-temporal/polyfill, and
                     * staticjs) counted from the unit above. They differ
                     * where the unit above holds an odd number of
                     * increments, below the hour: that case is left out,
                     * since diff-node's oracle is temporal-polyfill
                     * (docs/temporal.md, "Rounding"). */
                    bool differ = strcmp(MODES[m], "halfEven") == 0 && unit < 5 && (parts / increment) % 2 != 0;
                    text source = { 0 };
                    put_times(&source, dated, (quantum){ increment * UNIT_NS[unit], !differ });
                    char options[128];
                    snprintf(options, sizeof options, ".map(t => t.round({ smallestUnit: \"%s\", roundingIncrement: %lld, roundingMode: \"%s\" }))",
                             UNIT_NAMES[unit], increment, MODES[m]);
                    put(&source, options);
                    evaluate(r, &source, false);
                    free(source.ptr);
                }
            }
            /* The unit alone, singular and plural: halfExpand. */
            for (int plural = 0; plural < 2; plural++) {
                text source = { 0 };
                put_times(&source, dated, (quantum){ UNIT_NS[unit], true });
                char options[64];
                snprintf(options, sizeof options, ".map(t => t.round(\"%s%s\"))", UNIT_NAMES[unit], plural ? "s" : "");
                put(&source, options);
                evaluate(r, &source, false);
                free(source.ptr);
            }
        }
    }
}

/* Rounding at Temporal's limits, where a PlainDateTime faults if it
 * leaves them. */
static void rounding_limits(runner *r) {
    static const char *const edges[] = {
        "-271821, 4, 19, 0, 0, 0, 0, 0, 1", "-271821, 4, 19, 12", "275760, 9, 13, 23, 59, 59, 999, 999, 999", "275760, 9, 13, 12",
        "275760, 9, 13, 11, 59, 59, 999, 999, 999",
    };
    for (size_t e = 0; e < COUNT(edges); e++) {
        for (int unit = 0; unit < 7; unit++) {
            for (size_t m = 0; m < COUNT(MODES); m++) {
                char source[256];
                snprintf(source, sizeof source, "new Temporal.PlainDateTime(%s).round({ smallestUnit: \"%s\", roundingMode: \"%s\" })", edges[e],
                         UNIT_NAMES[unit], MODES[m]);
                evaluate_with(r, "%s", source, false);
            }
        }
    }
}

/* Increments that do and do not divide each unit's next, and units each
 * type does not round to. */
static void increments(runner *r) {
    static const char *const values[] = { "0", "1", "2", "5", "7", "12", "23", "24", "25", "30", "59", "60", "100", "125", "999", "1000", "1001", "1e9", "1e9 + 1", "2.9", "-1" };
    static const char *const units[] = { "nanosecond", "microsecond", "millisecond", "second", "minute", "hour", "day", "week", "month", "year", "auto" };
    static const char *const receivers[] = { "new Temporal.PlainTime(12, 30, 30, 500, 500, 500)", "new Temporal.PlainDateTime(2024, 1, 31, 12, 30, 30, 500, 500, 500)" };
    for (size_t t = 0; t < COUNT(receivers); t++) {
        for (size_t u = 0; u < COUNT(units); u++) {
            for (size_t v = 0; v < COUNT(values); v++) {
                char source[256];
                snprintf(source, sizeof source, "%s.round({ smallestUnit: \"%s\", roundingIncrement: %s })", receivers[t], units[u], values[v]);
                evaluate_with(r, "%s", source, false);
            }
        }
    }
}

/* toString's fractionalSecondDigits and smallestUnit by every rounding
 * mode, over times at the halves of each digit, and at the limits. */
static void to_string_sweep(runner *r) {
    static const char *const precisions[] = {
        "fractionalSecondDigits: 0", "fractionalSecondDigits: 1", "fractionalSecondDigits: 2", "fractionalSecondDigits: 3",
        "fractionalSecondDigits: 4", "fractionalSecondDigits: 5", "fractionalSecondDigits: 6", "fractionalSecondDigits: 7",
        "fractionalSecondDigits: 8", "fractionalSecondDigits: 9", "fractionalSecondDigits: \"auto\"", "smallestUnit: \"minute\"",
        "smallestUnit: \"second\"", "smallestUnit: \"millisecond\"", "smallestUnit: \"microsecond\"", "smallestUnit: \"nanosecond\"",
        "smallestUnit: \"minutes\"", "smallestUnit: \"seconds\"",
    };
    static const char *const times[] = {
        "0, 0", "12, 30, 45, 678, 901, 234", "12, 30, 45, 500", "12, 30, 45, 0, 0, 5", "12, 30, 59, 999, 999, 999", "23, 59, 59, 999, 999, 999",
        "12, 30, 45, 123, 500", "12, 30, 30", "12, 30, 45, 550", "12, 30, 45, 450", "12, 30, 45, 0, 0, 50", "12, 30, 45, 0, 0, 150",
        "12, 30, 29, 999, 999, 999", "12, 30",
    };
    for (int dated = 0; dated < 2; dated++) {
        for (size_t p = 0; p < COUNT(precisions); p++) {
            for (size_t m = 0; m < COUNT(MODES); m++) {
                text source = { 0 };
                put(&source, "[");
                for (size_t t = 0; t < COUNT(times); t++) {
                    put(&source, t > 0 ? ", " : "");
                    put(&source, dated ? "new Temporal.PlainDateTime(2024, 12, 31, " : "new Temporal.PlainTime(");
                    put(&source, times[t]);
                    put(&source, ")");
                }
                char options[128];
                snprintf(options, sizeof options, "].map(t => t.toString({ %s, roundingMode: \"%s\" }))", precisions[p], MODES[m]);
                put(&source, options);
                evaluate(r, &source, false);
                free(source.ptr);
            }
        }
    }
    static const char *const edges[] = { "-271821, 4, 19, 0, 0, 0, 0, 0, 1", "275760, 9, 13, 23, 59, 59, 999, 999, 999", "275760, 9, 13, 23, 59, 30" };
    for (size_t e = 0; e < COUNT(edges); e++) {
        for (size_t p = 0; p < COUNT(precisions); p++) {
            for (size_t m = 0; m < COUNT(MODES); m++) {
                char source[256];
                snprintf(source, sizeof source, "new Temporal.PlainDateTime(%s).toString({ %s, roundingMode: \"%s\", calendarName: \"always\" })", edges[e],
                         precisions[p], MODES[m]);
                evaluate_with(r, "%s", source, false);
            }
        }
    }
    static const char *const options[] = {
        "{ fractionalSecondDigits: 10 }", "{ fractionalSecondDigits: -1 }", "{ fractionalSecondDigits: 9.9 }", "{ fractionalSecondDigits: -0.1 }",
        "{ fractionalSecondDigits: \"2\" }", "{ fractionalSecondDigits: \"Auto\" }", "{ fractionalSecondDigits: null }",
        "{ fractionalSecondDigits: true }", "{ fractionalSecondDigits: NaN }", "{ fractionalSecondDigits: Infinity }",
        "{ smallestUnit: \"hour\" }", "{ smallestUnit: \"day\" }", "{ smallestUnit: \"Minute\" }", "{ smallestUnit: null }",
        "{ roundingMode: \"x\" }", "{ roundingMode: \"halfeven\" }", "{ calendarName: \"x\" }", "{ calendarName: \"always\" }",
        "{ roundingIncrement: 5 }", "{}", "[]", "null", "undefined", "\"minute\"",
    };
    static const char *const receivers[] = { "new Temporal.PlainTime(12, 30, 45, 678)", "new Temporal.PlainDateTime(2024, 1, 31, 12, 30, 45, 678)" };
    for (size_t t = 0; t < COUNT(receivers); t++) {
        for (size_t o = 0; o < COUNT(options); o++) {
            char source[256];
            snprintf(source, sizeof source, "%s.toString(%s)", receivers[t], options[o]);
            evaluate_with(r, "%s", source, false);
        }
    }
}

/* ------------------------------------------------------------------------
 * Durations
 */

/* A Duration and a relativeTo date on which the polyfills give different
 * values, left out: temporal-polyfill counts a month from the 31st, or
 * from February 29, to the end of the next month, where the specification
 * (and @js-temporal/polyfill) stops before it (every round and total of
 * the pair, `unit` NULL); and a total in months or years whose last bit
 * its floating-point division rounds differently (`unit`). */
static const struct {
    const char *duration;
    const char *relative_to;
    const char *unit;
} DIFFERING[] = {
    { "P1Y11M30DT23H59M59.999999999S", "2024-02-29", NULL },
    { "P1MT12H", "2024-01-31", NULL },
    { "P1MT12H", "2023-01-31", NULL },
    { "P1M15D", "2024-12-31", "month" },
    { "P1Y2M", "2024-01-31", "year" },
    { "P1Y2M", "2024-12-31", "year" },
    { "P45D", "2023-03-01", "month" },
};

static bool differing(const char *duration, const char *relative_to, const char *unit) {
    for (size_t i = 0; i < COUNT(DIFFERING); i++) {
        if (strcmp(duration, DIFFERING[i].duration) != 0 || strcmp(relative_to, DIFFERING[i].relative_to) != 0) continue;
        if (DIFFERING[i].unit == NULL || (unit != NULL && strcmp(unit, DIFFERING[i].unit) == 0)) return true;
    }
    return false;
}

/* ISO 8601 durations: a sign, P, date parts and time parts, each list
 * varied with the others, in both cases of their letters; then strings at
 * the limits and past the grammar. */
static void duration_strings(runner *r) {
    static const char *const signs[] = { "", "-", "+" };
    static const char *const dates[] = { "", "1Y", "2M", "3W", "4D", "1Y2M3W4D", "10Y11M", "1D1Y", "1.5D", "0D", "4294967295Y", "4294967296M",
                                         "104249991374D", "104249991375D" };
    static const char *const times[] = { "", "T", "T5H", "T6M", "T7S", "T7.5S", "T7,123456789S", "T7.1234567890S", "T1.5H", "T1.5M",
                                         "T5H6M7S", "T1.5H6M", "T5H1.5M7S", "T0.000000001S", "T2501999792984H", "T9007199254740991.999999999S",
                                         "T7S5H", "T.5S", "T1.S" };
    static const char *const others[] = {
        "P", "PT", "T1H", "1D", "--P1D", "+-P1D", "P1Y1Y", "PT1H1H", "P1H", "PT1D", "PT1Y", "P1DT1D", " P1D", "P1D ", "P1 D", "P1_000D",
        "P1e3D", "P-1D", "P+1D", "P1.D", "P4294967295M", "P4294967295W", "P4294967296W", "PT2501999792983H", "PT150119987579016M",
        "PT150119987579017M", "PT9007199254740992S", "PT9007199254740993S", "PT2501999792983.999999999H", "PT2501999792983.9H",
        "P104249991374DT7H36M31.999999999S", "P104249991374DT7H36M32S", "P99999999999999999999D", "P00000000000000000000001D",
        "PT18446744073709551616S", "PT0,1H", "PT0.999999999H", "PT0.999999999M", "P1dT1h", "P1Dt1H", "P1W1D", "P1Y1W", "P1YT",
        "PT1H1M1S1", "\xe2\x88\x92P1D", "P\xef\xbc\x91\x44", "PT1.000000000S", "PT10.0000000000S", "",
    };
    for (size_t s = 0; s < COUNT(signs); s++) {
        for (size_t d = 0; d < COUNT(dates); d++) {
            for (size_t t = 0; t < COUNT(times); t++) {
                for (int lower = 0; lower < 2; lower++) {
                    char string[96];
                    snprintf(string, sizeof string, "%s%s%s%s", signs[s], lower ? "p" : "P", dates[d], times[t]);
                    for (char *c = string; lower && *c != '\0'; c++) {
                        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
                    }
                    text source = { 0 };
                    put(&source, "Temporal.Duration.from(");
                    put_quoted(&source, string, strlen(string));
                    put(&source, ")");
                    evaluate(r, &source, false);
                    free(source.ptr);
                }
            }
        }
    }
    for (size_t o = 0; o < COUNT(others); o++) {
        text source = { 0 };
        put(&source, "Temporal.Duration.from(");
        put_quoted(&source, others[o], strlen(others[o]));
        put(&source, ")");
        evaluate(r, &source, false);
        free(source.ptr);
    }
}

/* Durations to round and total, around month ends and leap days. */
static const char *const DURATIONS[] = {
    "P1M15D", "-P1M15D", "P1Y2M", "P2M3W", "P1Y11M30DT23H59M59.999999999S", "-P1Y11M30DT23H59M59.999999999S", "PT36H", "-PT36H", "P45D",
    "P1W3DT12H", "P1MT12H", "PT1H30M45.5S",
};
static const char *const RELATIVE_TO[] = { "2024-01-31", "2024-02-29", "2023-01-31", "2024-12-31", "2023-03-01" };
static const char *const DURATION_UNITS[] = { "nanosecond", "microsecond", "millisecond", "second", "minute", "hour", "day", "week", "month", "year" };
#define ALL_MODES "[\"ceil\", \"floor\", \"expand\", \"trunc\", \"halfCeil\", \"halfFloor\", \"halfExpand\", \"halfTrunc\", \"halfEven\"]"

/* round relative to each date: every pair of units, every mode (in one
 * line), and increments of the unit when it is the largest. */
static void duration_round_relative(runner *r) {
    static const int units[] = { 0, 3, 4, 5, 6, 7, 8, 9 };
    for (size_t d = 0; d < COUNT(DURATIONS); d++) {
        for (size_t t = 0; t < COUNT(RELATIVE_TO); t++) {
            for (size_t l = 0; l < COUNT(units); l++) {
                for (size_t s = 0; s <= l && !differing(DURATIONS[d], RELATIVE_TO[t], NULL); s++) {
                    char source[512];
                    snprintf(source, sizeof source,
                             ALL_MODES ".map(m => Temporal.Duration.from(\"%s\").round({ largestUnit: \"%s\", smallestUnit: \"%s\", roundingMode: m, relativeTo: \"%s\" }))",
                             DURATIONS[d], DURATION_UNITS[units[l]], DURATION_UNITS[units[s]], RELATIVE_TO[t]);
                    evaluate_with(r, "%s", source, false);
                }
            }
            for (int unit = 6; unit < 10 && !differing(DURATIONS[d], RELATIVE_TO[t], NULL); unit++) {
                char source[512];
                snprintf(source, sizeof source,
                         "[2, 3, 7].flatMap(i => " ALL_MODES ".map(m => Temporal.Duration.from(\"%s\").round({ largestUnit: \"%s\", smallestUnit: \"%s\", roundingIncrement: i, roundingMode: m, relativeTo: \"%s\" })))",
                         DURATIONS[d], DURATION_UNITS[unit], DURATION_UNITS[unit], RELATIVE_TO[t]);
                evaluate_with(r, "%s", source, false);
            }
        }
    }
}

/* round without relativeTo: of 24-hour days and time, to every unit of a
 * day or less, by its increments, every mode, up to each largest unit. */
static void duration_round_plain(runner *r) {
    static const char *const durations[] = { "PT36H", "-PT36H", "P2DT11H59M59.999999999S", "-PT1H30M45.5S", "PT0.000000500S", "PT29M59.5S", "P10D",
                                             "PT9007199254740991.999999999S", "PT0S" };
    static const long long increments[][4] = { { 1, 2, 500, 0 }, { 1, 4, 250, 0 }, { 1, 8, 125, 0 }, { 1, 15, 30, 0 }, { 1, 12, 20, 0 }, { 1, 3, 8, 0 }, { 1, 2, 7, 0 } };
    static const char *const largest[] = { "auto", "day", "hour", "second", "nanosecond" };
    for (size_t d = 0; d < COUNT(durations); d++) {
        for (int unit = 0; unit < 7; unit++) {
            for (size_t i = 0; i < 3; i++) {
                for (size_t l = 0; l < COUNT(largest); l++) {
                    char source[512];
                    snprintf(source, sizeof source,
                             ALL_MODES ".map(m => Temporal.Duration.from(\"%s\").round({ largestUnit: \"%s\", smallestUnit: \"%s\", roundingIncrement: %lld, roundingMode: m }))",
                             durations[d], largest[l], DURATION_UNITS[unit], increments[unit][i]);
                    evaluate_with(r, "%s", source, false);
                }
            }
        }
    }
}

/* total in every unit, relative to each date and without one. */
static void duration_total(runner *r) {
    for (size_t d = 0; d < COUNT(DURATIONS); d++) {
        for (int unit = 0; unit < 10; unit++) {
            for (size_t t = 0; t <= COUNT(RELATIVE_TO); t++) {
                char source[512];
                if (t < COUNT(RELATIVE_TO) && differing(DURATIONS[d], RELATIVE_TO[t], DURATION_UNITS[unit])) continue;
                if (t == COUNT(RELATIVE_TO)) {
                    snprintf(source, sizeof source, "Temporal.Duration.from(\"%s\").total(\"%s\")", DURATIONS[d], DURATION_UNITS[unit]);
                } else {
                    snprintf(source, sizeof source, "Temporal.Duration.from(\"%s\").total({ unit: \"%s\", relativeTo: \"%s\" })", DURATIONS[d], DURATION_UNITS[unit],
                             RELATIVE_TO[t]);
                }
                evaluate_with(r, "%s", source, false);
            }
        }
    }
}

/* toString's digits and smallestUnit by every mode; compare relative to
 * each date; add and subtract. */
static void duration_text_and_arithmetic(runner *r) {
    static const char *const precisions[] = {
        "fractionalSecondDigits: 0", "fractionalSecondDigits: 1", "fractionalSecondDigits: 2", "fractionalSecondDigits: 3",
        "fractionalSecondDigits: 4", "fractionalSecondDigits: 5", "fractionalSecondDigits: 6", "fractionalSecondDigits: 7",
        "fractionalSecondDigits: 8", "fractionalSecondDigits: 9", "fractionalSecondDigits: \"auto\"", "smallestUnit: \"second\"",
        "smallestUnit: \"millisecond\"", "smallestUnit: \"microseconds\"", "smallestUnit: \"nanosecond\"", "smallestUnit: \"minute\"",
    };
    static const char *const texts[] = { "P1Y11M30DT23H59M59.999999999S", "-P1Y11M30DT23H59M59.999999999S", "PT59M59.5S", "-P1DT0.000000005S", "P1D",
                                         "PT0S", "PT1.050S", "PT9007199254740991.999999999S", "P3W", "PT1H0.000450S" };
    for (size_t t = 0; t < COUNT(texts); t++) {
        for (size_t p = 0; p < COUNT(precisions); p++) {
            char source[512];
            snprintf(source, sizeof source, ALL_MODES ".map(m => Temporal.Duration.from(\"%s\").toString({ %s, roundingMode: m }))", texts[t], precisions[p]);
            evaluate_with(r, "%s", source, false);
        }
    }
    for (size_t a = 0; a < COUNT(DURATIONS); a++) {
        for (size_t b = 0; b < COUNT(DURATIONS); b++) {
            char source[512];
            snprintf(source, sizeof source, "[\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"].map(r => Temporal.Duration.compare(\"%s\", \"%s\", { relativeTo: r }))",
                     RELATIVE_TO[0], RELATIVE_TO[1], RELATIVE_TO[2], RELATIVE_TO[3], RELATIVE_TO[4], DURATIONS[a], DURATIONS[b]);
            evaluate_with(r, "%s", source, false);
            snprintf(source, sizeof source, "Temporal.Duration.from(\"%s\").add(\"%s\")", DURATIONS[a], DURATIONS[b]);
            evaluate_with(r, "%s", source, false);
            snprintf(source, sizeof source, "Temporal.Duration.from(\"%s\").subtract(\"%s\")", DURATIONS[a], DURATIONS[b]);
            evaluate_with(r, "%s", source, false);
        }
    }
}

/* ------------------------------------------------------------------------
 * add, subtract, until and since of a PlainDate, a PlainTime and a
 * PlainDateTime
 */

/* Dates at the ends of months, around leap days and at the limits. */
static const char *const EDGE_DATES[] = { "2024-01-31", "2024-02-29", "2024-03-31", "2023-02-28", "2025-02-28", "2024-12-31",
                                          "2020-01-01", "1999-11-30", "2024-04-30", "-271821-04-19", "+275760-09-13", "2000-02-29" };
static const char *const DATE_UNITS[] = { "day", "week", "month", "year" };

/* until between every two of the dates over every pair of units, by
 * increments of 1, 2 and 5, and since by 1; every mode in one line. */
static void date_differences(runner *r) {
    for (size_t a = 0; a < COUNT(EDGE_DATES); a++) {
        for (size_t b = 0; b < COUNT(EDGE_DATES); b++) {
            for (int l = 0; l < 4; l++) {
                for (int s = 0; s <= l; s++) {
                    static const int increments[] = { 1, 2, 5, -1 };
                    for (size_t i = 0; i < COUNT(increments); i++) {
                        bool since = increments[i] < 0;
                        char source[512];
                        snprintf(source, sizeof source,
                                 ALL_MODES ".map(m => Temporal.PlainDate.from(\"%s\").%s(\"%s\", { largestUnit: \"%s\", smallestUnit: \"%s\", roundingIncrement: %d, roundingMode: m }))",
                                 EDGE_DATES[a], since ? "since" : "until", EDGE_DATES[b], DATE_UNITS[l], DATE_UNITS[s], since ? 1 : increments[i]);
                        evaluate_with(r, "%s", source, false);
                    }
                }
            }
        }
    }
}

/* until and since between dates and times on either side of each other's
 * time of day, over pairs of units from a year to a nanosecond. The
 * polyfills differ on months counted from the 29th to the 31st into a
 * shorter month (docs/temporal.md, "Differences, rounded"): rounded to a
 * month or a year from such a day, the line is left out. */
static void date_time_differences(runner *r) {
    static const char *const times[][2] = { { "T00:00", "T23:59:59.999999999" }, { "T12:00", "T06:30:15.5" }, { "T23:59:59.999999999", "T00:00" } };
    static const char *const units[] = { "nanosecond", "second", "minute", "hour", "day", "week", "month", "year" };
    static const int pairs[][2] = { { 7, 7 }, { 7, 6 }, { 7, 4 }, { 7, 0 }, { 6, 6 }, { 6, 4 }, { 6, 3 }, { 5, 5 }, { 5, 4 }, { 4, 4 }, { 4, 3 }, { 4, 0 }, { 3, 3 }, { 3, 2 }, { 2, 1 }, { 1, 1 }, { 0, 0 } };
    for (size_t a = 0; a < 9; a++) {
        for (size_t b = 0; b < 9; b++) {
            for (size_t t = 0; t < COUNT(times); t++) {
                for (size_t p = 0; p < COUNT(pairs); p++) {
                    int day = atoi(EDGE_DATES[a] + strlen(EDGE_DATES[a]) - 2);
                    if (pairs[p][1] >= 6 && day >= 29) continue;
                    for (int since = 0; since < 2; since++) {
                        char source[512];
                        snprintf(source, sizeof source,
                                 ALL_MODES ".map(m => Temporal.PlainDateTime.from(\"%s%s\").%s(\"%s%s\", { largestUnit: \"%s\", smallestUnit: \"%s\", roundingMode: m }))",
                                 EDGE_DATES[a], times[t][0], since ? "since" : "until", EDGE_DATES[b], times[t][1], units[pairs[p][0]], units[pairs[p][1]]);
                        evaluate_with(r, "%s", source, false);
                    }
                }
            }
        }
    }
    static const char *const limits[][2] = { { "-271821-04-19T00:00:00.000000001", "+275760-09-13T23:59:59.999999999" }, { "+275760-09-13T23:59:59.999999999", "-271821-04-20" } };
    for (size_t e = 0; e < COUNT(limits); e++) {
        for (size_t l = 0; l < COUNT(units); l++) {
            char source[512];
            snprintf(source, sizeof source, ALL_MODES ".map(m => Temporal.PlainDateTime.from(\"%s\").until(\"%s\", { largestUnit: \"%s\", roundingMode: m }))", limits[e][0],
                     limits[e][1], units[l]);
            evaluate_with(r, "%s", source, false);
        }
    }
}

/* until and since between times over every pair of units and every
 * increment that divides the larger unit. */
static void time_differences(runner *r) {
    static const char *const times[][2] = { { "00:00", "23:59:59.999999999" }, { "12:30:15.123456789", "01:02:03.5" }, { "06:00", "06:00:00.000000001" },
                                             { "23:30", "00:29:59.5" } };
    for (size_t t = 0; t < COUNT(times); t++) {
        for (int l = 0; l < 6; l++) {
            for (int s = 0; s <= l; s++) {
                long long parts = s == 5 ? 24 : s >= 3 ? 60 : 1000;
                for (long long increment = 1; increment < parts; increment++) {
                    if (parts % increment != 0) continue;
                    for (int since = 0; since < 2; since++) {
                        char source[512];
                        snprintf(source, sizeof source,
                                 ALL_MODES ".map(m => Temporal.PlainTime.from(\"%s\").%s(\"%s\", { largestUnit: \"%s\", smallestUnit: \"%s\", roundingIncrement: %lld, roundingMode: m }))",
                                 times[t][0], since ? "since" : "until", times[t][1], UNIT_NAMES[l], UNIT_NAMES[s], increment);
                        evaluate_with(r, "%s", source, false);
                    }
                }
            }
        }
    }
}

/* add and subtract of durations at month ends, leap days and the limits,
 * by each overflow. */
static void additions(runner *r) {
    static const char *const durations[] = { "P1M", "P1Y", "P1Y1M", "P13M", "P1M1D", "P1W", "P4Y", "P100Y", "P1D", "PT24H", "PT23H59M59.999999999S",
                                              "PT25H", "P1MT1H", "P1Y2M3W4DT5H6M7.008009010S", "P100000000D", "PT2400000000000H", "P4294967295W",
                                              "PT0S" };
    static const char *const overflows[] = { "", ", { overflow: \"constrain\" }", ", { overflow: \"reject\" }" };
    for (size_t d = 0; d < COUNT(EDGE_DATES); d++) {
        for (size_t u = 0; u < COUNT(durations); u++) {
            for (size_t o = 0; o < COUNT(overflows); o++) {
                char source[512];
                snprintf(source, sizeof source, "Temporal.PlainDate.from(\"%s\").add(\"%s\"%s)", EDGE_DATES[d], durations[u], overflows[o]);
                evaluate_with(r, "%s", source, false);
                snprintf(source, sizeof source, "Temporal.PlainDate.from(\"%s\").subtract(\"%s\"%s)", EDGE_DATES[d], durations[u], overflows[o]);
                evaluate_with(r, "%s", source, false);
                snprintf(source, sizeof source, "Temporal.PlainDateTime.from(\"%sT12:00\").add(\"%s\"%s)", EDGE_DATES[d], durations[u], overflows[o]);
                evaluate_with(r, "%s", source, false);
                snprintf(source, sizeof source, "Temporal.PlainDateTime.from(\"%sT23:59:59.999999999\").subtract(\"-%s\"%s)", EDGE_DATES[d], durations[u],
                         overflows[o]);
                evaluate_with(r, "%s", source, false);
            }
        }
    }
    static const char *const times[] = { "00:00", "12:30:15.5", "23:59:59.999999999" };
    for (size_t t = 0; t < COUNT(times); t++) {
        for (size_t u = 0; u < COUNT(durations); u++) {
            char source[512];
            snprintf(source, sizeof source, "[Temporal.PlainTime.from(\"%s\").add(\"%s\"), Temporal.PlainTime.from(\"%s\").subtract(\"%s\")]", times[t],
                     durations[u], times[t], durations[u]);
            evaluate_with(r, "%s", source, false);
        }
    }
}

/* ------------------------------------------------------------------------
 * Instant
 */

/* Instants to round and write: around 0 and the limits, before and after
 * the epoch, at and beside the halves of the units and of a day. */
static const char *const INSTANTS[] = {
    "0", "1", "-1", "500", "-500", "1500", "-1500", "-2500", "999999999", "-999999999", "-1000000000", "1500000000", "-1500000000",
    "30000000000", "-30000000000", "-90000000000", "1800000000000", "-5400000000000", "43200000000000", "-41400000000000",
    "-86399999999999", "1704067200123456789", "-1704067200123456789", "8639999999999999999999", "-8639999999999999999999",
    "8640000000000000000000", "-8640000000000000000000",
};

/* A decimal integer's value (the instants are below 2^73). */
static __int128 int128_of(const char *digits) {
    bool negative = *digits == '-';
    __int128 v = 0;
    for (const char *c = digits + (negative ? 1 : 0); *c != '\0'; c++) v = v * 10 + (*c - '0');
    return negative ? -v : v;
}

/* Do temporal-polyfill and the specification (and @js-temporal/polyfill)
 * round `epoch` differently? Below zero, at an odd quotient,
 * temporal-polyfill takes trunc and expand, and halfExpand and halfTrunc
 * on a tie, the other way (docs/temporal.md): staticjs faults there. */
static bool rounds_differently(__int128 epoch, long long increment, const char *mode) {
    __int128 q = epoch / increment;
    __int128 rest = epoch % increment;
    if (rest < 0) {
        rest += increment;
        q -= 1;
    }
    if (rest == 0 || q >= 0 || q % 2 == 0) return false;
    bool tie = rest * 2 == increment;
    return strcmp(mode, "trunc") == 0 || strcmp(mode, "expand") == 0 || (tie && (strcmp(mode, "halfExpand") == 0 || strcmp(mode, "halfTrunc") == 0));
}

/* `[new Temporal.Instant(...n), ...]` of the instants that both polyfills
 * round alike by `increment` and `mode`; the others into `left_out`. */
static void put_instants(text *t, long long increment, const char *mode, size_t *left_out) {
    put(t, "[");
    bool first = true;
    for (size_t i = 0; i < COUNT(INSTANTS); i++) {
        if (increment > 0 && rounds_differently(int128_of(INSTANTS[i]), increment, mode)) {
            if (left_out != NULL) left_out[i] = 1;
            continue;
        }
        put(t, first ? "" : ", ");
        put(t, "new Temporal.Instant(");
        put(t, INSTANTS[i]);
        put(t, "n)");
        first = false;
    }
    put(t, "]");
}

/* ISO 8601 strings for Temporal.Instant.from: dates, times, offsets and
 * annotations, each list varied with the others, then strings past the
 * grammar. An offset of 60 seconds is the part staticjs refuses on purpose:
 * temporal-polyfill reads it, @js-temporal/polyfill refuses it. */
static void instant_strings(runner *r) {
    static const char *const dates[] = { "2024-01-31", "19691231", "+275760-09-13", "-271821-04-20", "-271821-04-19", "2024-02-30" };
    static const char *const times[] = { "T00:00", "t12:30:45.123456789", " 23:59:60", "T12", "T123045,5", "T24:00", "" };
    static const part offsets[] = { { "Z", false }, { "z", false }, { "+01:00", false }, { "-01:00", false }, { "+0100", false }, { "-01", false },
                                    { "-23:59:59.999999999", false }, { "+00:00:30.5", false }, { "+01:00:60", true }, { "+01:0030", false },
                                    { "+24:00", false }, { "-00:00", false }, { "+01:00.5", false }, { "", false } };
    static const char *const annotations[] = { "", "[UTC]", "[!Europe/Paris]", "[u-ca=hebrew]", "[!foo=bar]", "[+01:00][u-ca=iso8601][x=y]" };
    for (size_t d = 0; d < COUNT(dates); d++) {
        for (size_t t = 0; t < COUNT(times); t++) {
            for (size_t o = 0; o < COUNT(offsets); o++) {
                for (size_t a = 0; a < COUNT(annotations); a++) {
                    char string[128];
                    snprintf(string, sizeof string, "%s%s%s%s", dates[d], times[t], offsets[o].text, annotations[a]);
                    text source = { 0 };
                    put(&source, "Temporal.Instant.from(");
                    put_quoted(&source, string, strlen(string));
                    put(&source, ")");
                    evaluate(r, &source, offsets[o].refused);
                    free(source.ptr);
                }
            }
        }
    }
    /* temporal-polyfill reads an empty annotation, an empty value and a
     * time zone after a calendar, which @js-temporal/polyfill refuses: the
     * parts staticjs refuses on purpose. */
    static const part others[] = {
        { "2024-01-31", false }, { "2024-01-31Z", false }, { "2024-01-31T00:00:00Z", false }, { "2024-01-31T00:00:00.000000000Z", false },
        { "2024-01-31T00:00:00.0000000001Z", false }, { "2024-01-31T00:00Z[u-ca=hebrew][u-ca=gregory]", false },
        { "2024-01-31T00:00Z[!u-ca=hebrew][u-ca=gregory]", false }, { "2024-01-31T00:00Z[u-ca=HEBREW]", false }, { "2024-01-31T00:00Z[+01:00:30]", false },
        { "2024-01-31T00:00Z[UTC][UTC]", false }, { "2024-01-31T00:00Z[u-ca=iso8601][UTC]", true }, { "2024-01-31T00:00\342\210\22201:00", false },
        { "-000000-01-01T00:00Z", false }, { "+275760-09-13T00:00:00.000000001Z", false }, { "+275760-09-13T01:00+01:00", false },
        { "+275760-09-13T01:00:00.000000001+01:00", false }, { "+275760-09-13T23:59+23:59", false }, { "-271821-04-19T23:00-01:00", false },
        { "-271821-04-19T23:59:59.999999999Z", false }, { "-271821-04-19T00:00:00.000000001-23:59", false }, { "+275760-09-14T00:00+23:59", false },
        { "2024-01-31T00:00ZZ", false }, { "2024-01-31T00:00Z ", false }, { " 2024-01-31T00:00Z", false }, { "2024-01-31T00:00+01:00Z", false },
        { "2024-01-31TZ", false }, { "2024-01-31T00:00Z[]", true }, { "2024-01-31T00:00Z[u-ca=]", true }, { "2024-01-31T00:00:00,5+01:00:00,5", false },
        { "2024-01-31T00:00+1", false }, { "1970-01-01T00:00:00.000000001Z", false }, { "1969-12-31T23:59:59.999999999Z", false }, { "", false },
    };
    for (size_t o = 0; o < COUNT(others); o++) {
        text source = { 0 };
        put(&source, "Temporal.Instant.from(");
        put_quoted(&source, others[o].text, strlen(others[o].text));
        put(&source, ")");
        evaluate(r, &source, others[o].refused);
        free(source.ptr);
    }
}

/* The constructor and the functions of epochs: BigInts, numbers and other
 * values, at and past the limits; and the getters, of negative instants
 * past a millisecond too. */
static void instant_epochs(runner *r) {
    static const part bigints[] = { { "0n", false }, { "1n", false }, { "-1n", false }, { "8640000000000000000000n", false },
                                    { "8640000000000000000001n", false }, { "-8640000000000000000000n", false }, { "-8640000000000000000001n", false },
                                    { "2n ** 96n", false }, { "-(2n ** 72n)", false }, { "true", false }, { "false", false }, { "1", false },
                                    { "null", false }, { "undefined", false }, { "\"5\"", true }, { "[]", true }, { "{}", true } };
    for (size_t b = 0; b < COUNT(bigints); b++) {
        evaluate_with(r, "new Temporal.Instant(%s)", bigints[b].text, bigints[b].refused);
        evaluate_with(r, "Temporal.Instant.fromEpochNanoseconds(%s)", bigints[b].text, bigints[b].refused);
    }
    static const part milliseconds[] = { { "0", false }, { "-0", false }, { "1", false }, { "-1", false }, { "1.5", false }, { "-0.5", false },
                                         { "8.64e15", false }, { "8.64e15 + 1", false }, { "-8.64e15", false }, { "-8.64e15 - 1", false },
                                         { "NaN", false }, { "Infinity", false }, { "true", false }, { "null", false }, { "undefined", false },
                                         { "5n", false }, { "\"5\"", true }, { "[5]", true }, { "1704067200123", false } };
    for (size_t m = 0; m < COUNT(milliseconds); m++) {
        evaluate_with(r, "Temporal.Instant.fromEpochMilliseconds(%s)", milliseconds[m].text, milliseconds[m].refused);
    }
    static const char *const epochs[] = { "0n", "-1n", "-999999n", "-1000000n", "-1000001n", "-1999999n", "1999999n", "-1500000000n",
                                          "-8640000000000000000000n", "8640000000000000000000n", "-8639999999999999999999n", "-1704067200123456789n" };
    for (size_t e = 0; e < COUNT(epochs); e++) {
        evaluate_with(r, "(i => [i.epochMilliseconds, i.epochNanoseconds, i.toString(), i.toJSON(), JSON.stringify(i)])(new Temporal.Instant(%s))", epochs[e],
                      false);
    }
}

/* round over every unit, increments that divide a day's worth of it and
 * some that do not, every mode, of each instant; and the deliberate
 * faults, where the polyfills round differently. */
static void instant_rounding(runner *r) {
    static const long long increments[6][9] = {
        { 1, 2, 3, 1000, 864000000, 1000000000, 999999999, 7, 0 },
        { 1, 5, 1000, 864000000, 1000000000, 86400000000, 7, 0 },
        { 1, 2, 8, 640, 1000, 86400000, 7, 0 },
        { 1, 2, 30, 60, 3600, 43200, 86400, 7, 172800 },
        { 1, 2, 15, 45, 90, 720, 1440, 7, 2880 },
        { 1, 2, 3, 4, 6, 8, 12, 24, 5 },
    };
    for (int unit = 0; unit < 6; unit++) {
        for (size_t i = 0; i < 9 && increments[unit][i] != 0; i++) {
            long long increment = increments[unit][i];
            bool divides = increment <= DAY_NS / UNIT_NS[unit] && (DAY_NS / UNIT_NS[unit]) % increment == 0;
            for (size_t m = 0; m < COUNT(MODES); m++) {
                size_t left_out[COUNT(INSTANTS)] = { 0 };
                text source = { 0 };
                put_instants(&source, divides ? increment * UNIT_NS[unit] : 0, MODES[m], left_out);
                char options[160];
                snprintf(options, sizeof options, ".map(i => i.round({ smallestUnit: \"%s\", roundingIncrement: %lld, roundingMode: \"%s\" }))", UNIT_NAMES[unit],
                         increment, MODES[m]);
                put(&source, options);
                /* Past 10^9, temporal-polyfill rounds, where the
                 * specification (and @js-temporal/polyfill) throws. */
                evaluate(r, &source, increment > 1000000000);
                free(source.ptr);
                for (size_t k = 0; k < COUNT(INSTANTS) && i == 1; k++) {
                    if (!left_out[k]) continue;
                    char deliberate[256];
                    snprintf(deliberate, sizeof deliberate, "new Temporal.Instant(%sn).round({ smallestUnit: \"%s\", roundingIncrement: %lld, roundingMode: \"%s\" })",
                             INSTANTS[k], UNIT_NAMES[unit], increment, MODES[m]);
                    evaluate_with(r, "%s", deliberate, true);
                }
            }
        }
        for (int plural = 0; plural < 2; plural++) {
            text source = { 0 };
            put_instants(&source, UNIT_NS[unit], "halfExpand", NULL);
            char options[64];
            snprintf(options, sizeof options, ".map(i => i.round(\"%s%s\"))", UNIT_NAMES[unit], plural ? "s" : "");
            put(&source, options);
            evaluate(r, &source, false);
            free(source.ptr);
        }
    }
    static const char *const options[] = { "", "{}", "\"day\"", "{ smallestUnit: \"day\" }", "{ smallestUnit: \"week\" }", "{ smallestUnit: \"auto\" }",
                                           "{ smallestUnit: \"hour\", roundingIncrement: 0 }", "{ smallestUnit: \"hour\", roundingIncrement: 2.9 }",
                                           "{ smallestUnit: \"hour\", roundingMode: \"halfeven\" }", "{ smallestUnit: \"Hour\" }", "null", "5",
                                           "{ roundingIncrement: 2 }", "{ smallestUnit: \"nanosecond\", roundingIncrement: 1e9 + 1 }" };
    for (size_t o = 0; o < COUNT(options); o++) evaluate_with(r, "new Temporal.Instant(1500n).round(%s)", options[o], false);
}

/* toString's fractionalSecondDigits and smallestUnit by every mode; its
 * time zones; and the deliberate faults where the polyfills round
 * differently. */
static void instant_text(runner *r) {
    static const struct {
        const char *option;
        long long increment;
    } precisions[] = {
        { "fractionalSecondDigits: 0", 1000000000 }, { "fractionalSecondDigits: 1", 100000000 }, { "fractionalSecondDigits: 2", 10000000 },
        { "fractionalSecondDigits: 3", 1000000 }, { "fractionalSecondDigits: 4", 100000 }, { "fractionalSecondDigits: 5", 10000 },
        { "fractionalSecondDigits: 6", 1000 }, { "fractionalSecondDigits: 7", 100 }, { "fractionalSecondDigits: 8", 10 },
        { "fractionalSecondDigits: 9", 1 }, { "fractionalSecondDigits: \"auto\"", 1 }, { "smallestUnit: \"minute\"", 60000000000 },
        { "smallestUnit: \"second\"", 1000000000 }, { "smallestUnit: \"millisecond\"", 1000000 }, { "smallestUnit: \"microseconds\"", 1000 },
        { "smallestUnit: \"nanosecond\"", 1 },
    };
    for (size_t p = 0; p < COUNT(precisions); p++) {
        for (size_t m = 0; m < COUNT(MODES); m++) {
            size_t left_out[COUNT(INSTANTS)] = { 0 };
            text source = { 0 };
            put_instants(&source, precisions[p].increment, MODES[m], left_out);
            char options[128];
            snprintf(options, sizeof options, ".map(i => i.toString({ %s, roundingMode: \"%s\" }))", precisions[p].option, MODES[m]);
            put(&source, options);
            evaluate(r, &source, false);
            free(source.ptr);
            for (size_t k = 0; k < COUNT(INSTANTS) && p == 3; k++) {
                if (!left_out[k]) continue;
                char deliberate[256];
                snprintf(deliberate, sizeof deliberate, "new Temporal.Instant(%sn).toString({ %s, roundingMode: \"%s\" })", INSTANTS[k], precisions[p].option,
                         MODES[m]);
                evaluate_with(r, "%s", deliberate, true);
            }
        }
    }
    static const part zones[] = {
        { "\"UTC\"", false }, { "\"utc\"", false }, { "\"uTc\"", false }, { "\"+01:00\"", false }, { "\"-01:30\"", false }, { "\"+0100\"", false },
        { "\"+01\"", false }, { "\"-00:00\"", false }, { "\"+23:59\"", false }, { "\"-2359\"", false }, { "\"+24:00\"", false }, { "\"+1\"", false },
        { "\"+01:0\"", false }, { "\"+01:60\"", true }, { "\"+01:00:30\"", false }, { "\"+01:00.5\"", false }, { "\"\"", false }, { "\"Z\"", false },
        { "\"UTC[u-ca=iso8601]\"", false }, { "\"Etc/UTC\"", true }, { "\"GMT\"", true }, { "\"Europe/Paris\"", true }, { "\"2020-01-01T00:00Z\"", true },
        { "\"2020-01-01T00:00+01:00\"", true }, { "\"1970-01-01\"", false }, { "5", false }, { "null", false }, { "{}", false }, { "undefined", false },
    };
    static const char *const receivers[] = { "0n", "1500n", "-1n", "8640000000000000000000n", "-8640000000000000000000n" };
    for (size_t z = 0; z < COUNT(zones); z++) {
        for (size_t i = 0; i < COUNT(receivers); i++) {
            char source[256];
            snprintf(source, sizeof source, "new Temporal.Instant(%s).toString({ timeZone: %s, smallestUnit: \"microsecond\", roundingMode: \"ceil\" })",
                     receivers[i], zones[z].text);
            evaluate_with(r, "%s", source, zones[z].refused);
        }
        evaluate_with(r, "new Temporal.Instant(-1704067200123456789n).toString({ timeZone: %s })", zones[z].text, zones[z].refused);
    }
    static const char *const options[] = {
        "{ fractionalSecondDigits: 10 }", "{ fractionalSecondDigits: -1 }", "{ fractionalSecondDigits: 9.9 }", "{ fractionalSecondDigits: \"2\" }",
        "{ fractionalSecondDigits: null }", "{ smallestUnit: \"hour\" }", "{ smallestUnit: \"day\" }", "{ smallestUnit: \"Minute\" }",
        "{ roundingMode: \"x\" }", "{ calendarName: \"always\" }", "{ roundingIncrement: 5 }", "{}", "[]", "null", "undefined", "\"minute\"",
    };
    for (size_t o = 0; o < COUNT(options); o++) evaluate_with(r, "new Temporal.Instant(1704067200123456789n).toString(%s)", options[o], false);
}

/* until and since over pairs of instants, every pair of units, increment
 * and mode (in one line); the defaults; add and subtract. */
static void instant_arithmetic(runner *r) {
    static const char *const pairs[][2] = { { "0n", "3723004005006n" }, { "-1500n", "1704067200123456789n" },
                                            { "-8640000000000000000000n", "8640000000000000000000n" } };
    for (size_t p = 0; p < COUNT(pairs); p++) {
        for (int l = 0; l < 6; l++) {
            for (int s = 0; s <= l; s++) {
                long long parts = s == 5 ? 24 : s >= 3 ? 60 : 1000;
                for (long long increment = 1; increment < parts; increment++) {
                    if (parts % increment != 0) continue;
                    for (int since = 0; since < 2; since++) {
                        char source[512];
                        snprintf(source, sizeof source,
                                 ALL_MODES ".map(m => new Temporal.Instant(%s).%s(new Temporal.Instant(%s), { largestUnit: \"%s\", smallestUnit: \"%s\", roundingIncrement: %lld, roundingMode: m }))",
                                 pairs[p][0], since ? "since" : "until", pairs[p][1], UNIT_NAMES[l], UNIT_NAMES[s], increment);
                        evaluate_with(r, "%s", source, false);
                    }
                }
            }
        }
    }
    static const char *const options[] = { "", "{}", "{ largestUnit: \"auto\" }", "{ smallestUnit: \"minute\" }", "{ largestUnit: \"minutes\" }",
                                            "{ largestUnit: \"day\" }", "{ smallestUnit: \"day\" }", "{ largestUnit: \"second\", smallestUnit: \"hour\" }",
                                            "{ smallestUnit: \"hour\", roundingIncrement: 24 }", "{ smallestUnit: \"minute\", roundingIncrement: 7 }",
                                            "{ roundingMode: \"halfEven\", smallestUnit: \"second\" }", "null", "\"hour\"" };
    static const char *const others[] = { "new Temporal.Instant(3723004005006n)", "\"1970-01-01T01:02:03.004005006Z\"", "\"1970-01-01T02:02:03.004005006+01:00\"",
                                          "\"1970-01-01T01:02:03\"", "3723004005006n", "new Temporal.PlainDateTime(1970, 1, 1)" };
    for (size_t o = 0; o < COUNT(options); o++) {
        for (size_t t = 0; t < COUNT(others); t++) {
            char source[512];
            snprintf(source, sizeof source, "[new Temporal.Instant(-1n).until(%s%s%s), new Temporal.Instant(-1n).since(%s%s%s)]", others[t],
                     options[o][0] ? ", " : "", options[o], others[t], options[o][0] ? ", " : "", options[o]);
            evaluate_with(r, "%s", source, false);
        }
    }
    static const char *const durations[] = { "\"PT1H\"", "\"-PT1H\"", "\"PT0.000000001S\"", "\"P1D\"", "\"PT24H\"", "\"P1Y\"", "\"P1W\"", "\"P1M\"",
                                             "{ hours: 1, nanoseconds: -0 }", "{ days: 0, minutes: 90 }", "{ nanoseconds: 8.64e21 }",
                                             "{ seconds: 9007199254740991, nanoseconds: 999999999 }", "{ hours: 2400000000000 }",
                                             "\"PT2399999999999H\"", "Temporal.Duration.from(\"PT1M\")", "{}", "5", "\"x\"" };
    static const char *const instants[] = { "0n", "-1n", "8640000000000000000000n", "-8640000000000000000000n", "1704067200123456789n" };
    for (size_t d = 0; d < COUNT(durations); d++) {
        for (size_t i = 0; i < COUNT(instants); i++) {
            char source[512];
            snprintf(source, sizeof source, "[new Temporal.Instant(%s).add(%s), new Temporal.Instant(%s).subtract(%s)]", instants[i], durations[d],
                     instants[i], durations[d]);
            evaluate_with(r, "%s", source, false);
        }
    }
}

/* compare and equals, of instants and strings, and other values. */
static void instant_comparisons(runner *r) {
    static const part values[] = { { "new Temporal.Instant(0n)", false }, { "new Temporal.Instant(-1n)", false },
                                   { "\"1970-01-01T00:00Z\"", false }, { "\"1970-01-01T01:00+01:00\"", false },
                                   { "\"1969-12-31T23:59:59.999999999Z\"", false }, { "\"1970-01-01T00:00\"", false }, { "0n", false },
                                   { "0", false }, { "null", false }, { "{}", false }, { "[\"1970-01-01T00:00Z\"]", true },
                                   { "Temporal.PlainDateTime.from(\"1970-01-01\")", false } };
    for (size_t a = 0; a < COUNT(values); a++) {
        for (size_t b = 0; b < COUNT(values); b++) {
            char source[512];
            snprintf(source, sizeof source, "Temporal.Instant.compare(%s, %s)", values[a].text, values[b].text);
            evaluate_with(r, "%s", source, values[a].refused || values[b].refused);
        }
        evaluate_with(r, "new Temporal.Instant(0n).equals(%s)", values[a].text, values[a].refused);
        evaluate_with(r, "Temporal.Instant.from(%s)", values[a].text, values[a].refused);
    }
}

/* ------------------------------------------------------------------------
 * Date (docs/date.md): Date.parse over strings of the date time string
 * format and beside it, Date.UTC over its fields' edges, and the getters,
 * toISOString, toJSON and toUTCString over times at the edges of years,
 * centuries and the limits. diff-node compares them with Node's own Date
 * (TZ=UTC). A string staticjs does not read is a deliberate fault: Node
 * reads many by heuristics of its own, and gives NaN for the rest.
 */

/* A part of a date time string, and whether the format has it (`valid`);
 * `value` is the year, month or day it gives, 0 for none. */
typedef struct {
    const char *text;
    bool valid;
    long value;
} date_part;

static const date_part DATE_YEARS[] = {
    { "2024", 1, 2024 }, { "1970", 1, 1970 }, { "1969", 1, 1969 }, { "0000", 1, 0 }, { "9999", 1, 9999 }, { "1900", 1, 1900 },
    { "2000", 1, 2000 }, { "2100", 1, 2100 }, { "+002024", 1, 2024 }, { "-000001", 1, -1 }, { "+275760", 1, 275760 },
    { "-271821", 1, -271821 }, { "+275761", 1, 275761 }, { "-271822", 1, -271822 }, { "-000000", 0, 0 }, { "+000000", 1, 0 },
    { "+010000", 1, 10000 }, { "-010000", 1, -10000 }, { "+999999", 1, 999999 }, { "-999999", 1, -999999 }, { "-000100", 1, -100 },
    { "-000400", 1, -400 }, { "202", 0, 0 }, { "20240", 0, 0 }, { "+02024", 0, 0 }, { "+2024", 0, 0 }, { "-2024", 0, 0 }, { "x024", 0, 0 },
    { "", 0, 0 },
};
static const date_part DATE_MONTHS[] = {
    { "", 1, 0 }, { "-01", 1, 1 }, { "-02", 1, 2 }, { "-04", 1, 4 }, { "-09", 1, 9 }, { "-12", 1, 12 }, { "-13", 0, 0 },
    { "-00", 0, 0 }, { "-1", 0, 0 }, { "-012", 0, 0 }, { "01", 0, 0 }, { "/01", 0, 0 },
};
static const date_part DATE_DAYS[] = {
    { "", 1, 0 }, { "-01", 1, 1 }, { "-13", 1, 13 }, { "-19", 1, 19 }, { "-20", 1, 20 }, { "-28", 1, 28 }, { "-29", 1, 29 },
    { "-30", 1, 30 }, { "-31", 1, 31 }, { "-32", 0, 0 }, { "-00", 0, 0 }, { "-1", 0, 0 }, { "-001", 0, 0 }, { "/01", 0, 0 },
};
static const date_part DATE_TIMES[] = {
    { "", 1, 0 }, { "T00:00", 1, 1 }, { "T12:30", 1, 1 }, { "T23:59:59", 1, 1 }, { "T23:59:59.999", 1, 1 }, { "T00:00:00.000", 1, 1 },
    { "T24:00", 1, 1 }, { "T24:00:00", 1, 1 }, { "T24:00:00.000", 1, 1 }, { "T24:00:01", 0, 0 }, { "T24:01", 0, 0 },
    { "T24:00:00.001", 0, 0 }, { "T25:00", 0, 0 }, { "T12:60", 0, 0 }, { "T12:30:60", 0, 0 }, { "T12:30:15.1", 0, 0 },
    { "T12:30:15.12", 0, 0 }, { "T12:30:15.1234", 0, 0 }, { "T12:30:15.", 0, 0 }, { "T12", 0, 0 }, { "T1230", 0, 0 },
    { "T1:30", 0, 0 }, { "t12:30", 0, 0 }, { " 12:30", 0, 0 }, { "T12:30:15,123", 0, 0 }, { "T", 0, 0 }, { "T12:3", 0, 0 },
};
static const date_part DATE_ZONES[] = {
    { "", 1, 0 }, { "Z", 1, 1 }, { "+00:00", 1, 1 }, { "-00:00", 1, 1 }, { "+05:30", 1, 1 }, { "-08:00", 1, 1 }, { "+23:59", 1, 1 },
    { "-23:59", 1, 1 }, { "+00:01", 1, 1 }, { "-00:01", 1, 1 }, { "z", 0, 0 }, { "+24:00", 0, 0 }, { "+05:60", 0, 0 },
    { "+05", 0, 0 }, { "+0530", 0, 0 }, { "+5:30", 0, 0 }, { "Z ", 0, 0 }, { " Z", 0, 0 }, { "GMT", 0, 0 }, { "UTC", 0, 0 },
    { "+05:30:00", 0, 0 }, { "Z[UTC]", 0, 0 }, { "+", 0, 0 },
};

static bool date_leap(long year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static void date_parse_case(runner *r, strings *s, const date_part *parts[5]) {
    static const long DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    text string = { 0 };
    put(&string, "");
    bool valid = true;
    for (int i = 0; i < 5; i++) {
        put(&string, parts[i]->text);
        valid = valid && parts[i]->valid;
    }
    long month = parts[1]->value, day = parts[2]->value;
    if (day != 0 && (month == 0 || day > DAYS[month - 1] + (month == 2 && date_leap(parts[0]->value) ? 1 : 0))) valid = false;
    if (parts[2]->text[0] != '\0' && parts[1]->text[0] == '\0') valid = false;
    /* A time needs Z or an offset (without, it is local time); an offset
     * needs a time. */
    if ((parts[3]->text[0] != '\0') != (parts[4]->text[0] != '\0')) valid = false;
    if (!seen_before(s, &string)) {
        text source = { 0 };
        put(&source, s->function);
        put(&source, "(");
        put_quoted(&source, string.ptr, string.length);
        put(&source, ")");
        evaluate(r, &source, !valid);
        free(source.ptr);
    }
    free(string.ptr);
}

/* Date.parse: the years, dates, times and zones crossed (some of each),
 * then every part varied alone around three strings. */
static void date_strings(runner *r) {
    static const size_t years[] = { 0, 1, 3, 4, 6, 8, 9, 10, 11, 12, 13, 14, 16, 17, 21, 22 };
    static const size_t dates[][2] = { { 0, 0 }, { 2, 0 }, { 2, 6 }, { 2, 7 }, { 3, 8 }, { 3, 7 }, { 5, 8 }, { 4, 5 } };
    static const size_t times[] = { 0, 1, 4, 6, 9, 15, 19 };
    static const size_t zones[] = { 0, 1, 4, 5, 6, 13 };
    static const date_part none = { "", 1, 0 };
    strings s = { { 0 }, "Date.parse" };
    for (size_t y = 0; y < COUNT(years); y++) {
        for (size_t d = 0; d < COUNT(dates); d++) {
            for (size_t t = 0; t < COUNT(times); t++) {
                for (size_t z = 0; z < COUNT(zones); z++) {
                    const date_part *parts[5] = { &DATE_YEARS[years[y]], &DATE_MONTHS[dates[d][0]], &DATE_DAYS[dates[d][1]], &DATE_TIMES[times[t]], &DATE_ZONES[zones[z]] };
                    date_parse_case(r, &s, parts);
                }
            }
        }
    }
    static const size_t bases[][5] = { { 0, 1, 8, 2, 1 }, { 10, 5, 0, 0, 0 }, { 11, 3, 4, 4, 9 } };
    for (size_t b = 0; b < COUNT(bases); b++) {
        const date_part *parts[5] = { &DATE_YEARS[bases[b][0]], &DATE_MONTHS[bases[b][1]], &DATE_DAYS[bases[b][2]], &DATE_TIMES[bases[b][3]], &DATE_ZONES[bases[b][4]] };
        for (size_t i = 0; i < COUNT(DATE_YEARS); i++) date_parse_case(r, &s, (const date_part *[5]){ &DATE_YEARS[i], parts[1], parts[2], parts[3], parts[4] });
        for (size_t i = 0; i < COUNT(DATE_MONTHS); i++) date_parse_case(r, &s, (const date_part *[5]){ parts[0], &DATE_MONTHS[i], parts[2], parts[3], parts[4] });
        for (size_t i = 0; i < COUNT(DATE_DAYS); i++) date_parse_case(r, &s, (const date_part *[5]){ parts[0], parts[1], &DATE_DAYS[i], parts[3], parts[4] });
        for (size_t i = 0; i < COUNT(DATE_TIMES); i++) date_parse_case(r, &s, (const date_part *[5]){ parts[0], parts[1], parts[2], &DATE_TIMES[i], parts[4] });
        for (size_t i = 0; i < COUNT(DATE_ZONES); i++) date_parse_case(r, &s, (const date_part *[5]){ parts[0], parts[1], parts[2], parts[3], &DATE_ZONES[i] });
    }
    /* Every day of every month in a leap year and a common one, at the
     * days past their ends. */
    static const date_part leap = { "2024", 1, 2024 }, common = { "2023", 1, 2023 }, century = { "1900", 1, 1900 };
    for (size_t m = 1; m <= 6; m++) {
        for (size_t d = 5; d <= 9; d++) {
            date_parse_case(r, &s, (const date_part *[5]){ &leap, &DATE_MONTHS[m], &DATE_DAYS[d], &none, &none });
            date_parse_case(r, &s, (const date_part *[5]){ &common, &DATE_MONTHS[m], &DATE_DAYS[d], &none, &none });
            date_parse_case(r, &s, (const date_part *[5]){ &century, &DATE_MONTHS[m], &DATE_DAYS[d], &none, &none });
        }
    }
    free(s.seen.ptr);
}

static void evaluate_text(runner *r, const char *source) {
    text t = { 0 };
    put(&t, source);
    evaluate(r, &t, false);
    free(t.ptr);
}

/* Date.UTC: years around 0, the 1900 to 1999 mapping, the limits and V8's
 * ranges, by months and days that balance; then each time field alone. */
static void date_utc(runner *r) {
    static const char *const years[] = {
        "-1000001", "-1000000", "-271822", "-271821", "-271820", "-10000", "-401", "-400", "-101", "-100", "-1", "-0.5", "0", "0.5",
        "1", "4", "99", "99.9", "100", "101", "1899", "1900", "1969", "1970", "1999", "2000", "2024", "2100", "9999", "10000",
        "275759", "275760", "275761", "1000000", "1000000.5", "NaN", "Infinity", "null", "true", "undefined",
    };
    static const char *const months[] = { "-10000001", "-10000000", "-13", "-12", "-1", "-0.5", "0", "1", "1.9", "11", "12", "13", "24", "10000000", "10000000.5" };
    static const char *const days[] = { "-100000001", "-1e8", "-366", "-1", "0", "1", "28", "29", "31", "32", "365", "1e8", "1e9", "NaN", "-Infinity" };
    char room[256];
    for (size_t y = 0; y < COUNT(years); y++) {
        for (size_t m = 0; m < COUNT(months); m++) {
            for (size_t d = 0; d < COUNT(days); d++) {
                snprintf(room, sizeof room, "Date.UTC(%s, %s, %s)", years[y], months[m], days[d]);
                evaluate_text(r, room);
            }
        }
        snprintf(room, sizeof room, "Date.UTC(%s)", years[y]);
        evaluate_text(r, room);
    }
    static const char *const fields[] = {
        "-1", "0", "0.5", "-0.5", "1", "23", "24", "59", "60", "999", "1000", "86400000", "1e9", "2.4e9", "8.64e15", "-8.64e15", "1e20", "1e300",
        "NaN", "Infinity", "-Infinity", "null", "undefined", "true",
    };
    static const char *const dates[] = { "1970, 0, 1", "-271821, 3, 20", "275760, 8, 12", "2024, 1, 29" };
    for (size_t d = 0; d < COUNT(dates); d++) {
        for (size_t f = 0; f < COUNT(fields); f++) {
            for (size_t at = 0; at < 4; at++) {
                const char *v[4] = { "0", "0", "0", "0" };
                v[at] = fields[f];
                snprintf(room, sizeof room, "Date.UTC(%s, %s, %s, %s, %s)", dates[d], v[0], v[1], v[2], v[3]);
                evaluate_text(r, room);
            }
        }
    }
}

/* The getters, toISOString, toJSON and toUTCString: times at the ends of
 * years and of February, a millisecond either side, in years around 0,
 * the centuries, 1970 and the limits; and time values at the limits. */
static void date_getters(runner *r) {
    static const char *const years[] = {
        "-271821", "-271820", "-100000", "-10000", "-1001", "-401", "-400", "-399", "-101", "-100", "-5", "-4", "-1", "0", "1",
        "4", "100", "400", "999", "1000", "1600", "1700", "1899", "1900", "1901", "1968", "1969", "1970", "1971", "1999", "2000",
        "2023", "2024", "2100", "9999", "10000", "99999", "275759", "275760",
    };
    static const char *const points[] = {
        "0, 1", "0, 1, 0, 0, 0, -1", "0, 1, 0, 0, 0, 1", "1, 28, 23, 59, 59, 999", "1, 29, 12", "2, 1", "2, 1, 0, 0, 0, -1",
        "11, 31, 23, 59, 59, 999", "11, 31, 24", "3, 19, 23, 59, 59, 999", "3, 20", "8, 13", "8, 12, 23, 59, 59, 999",
    };
    static const char *const getters =
        "(d => [d.getTime(), d.valueOf(), d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCDay(), d.getUTCHours(), "
        "d.getUTCMinutes(), d.getUTCSeconds(), d.getUTCMilliseconds(), d.toJSON(), d.toUTCString(), d.toGMTString(), d.toISOString()])";
    char room[512];
    for (size_t y = 0; y < COUNT(years); y++) {
        for (size_t p = 0; p < COUNT(points); p++) {
            snprintf(room, sizeof room, "%s(new Date(Date.UTC(%s, %s)))", getters, years[y], points[p]);
            evaluate_text(r, room);
        }
    }
    static const char *const times[] = {
        "0", "-1", "1", "-0", "86399999", "86400000", "-86400000", "-86400001", "8.64e15", "-8.64e15", "8.64e15 - 1", "-8.64e15 + 1",
        "8.64e15 + 1", "-8.64e15 - 1", "-62198755200000", "-62198755200001", "-62167219200000", "-62167219200001", "253402300799999",
        "253402300800000", "-2208988800000", "951782400000", "1e15", "-1e15", "123.9", "-123.9", "NaN",
    };
    for (size_t t = 0; t < COUNT(times); t++) {
        snprintf(room, sizeof room, "%s(new Date(%s))", getters, times[t]);
        evaluate_text(r, room);
    }
}

int main(int argc, char **argv) {
    runner r = { 0 };
    r.print = argc == 2 && strcmp(argv[1], "--print") == 0;
    sjs_context_config config = { { NULL, &counted_vtable }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&r.context, &config) != SJS_OK) abort();
    if (sjs_vm_init(&r.vm, &(sjs_vm_config){ r.context, false, false }) != SJS_OK) abort();
    iso_strings(&r, "Temporal.PlainDate.from");
    iso_strings(&r, "Temporal.PlainTime.from");
    iso_strings(&r, "Temporal.PlainDateTime.from");
    time_strings(&r);
    time_bags(&r);
    constructors(&r);
    rounding_sweep(&r);
    rounding_limits(&r);
    increments(&r);
    to_string_sweep(&r);
    getters(&r);
    limits(&r);
    bags(&r);
    calendar_names(&r);
    duration_strings(&r);
    duration_round_relative(&r);
    duration_round_plain(&r);
    duration_total(&r);
    duration_text_and_arithmetic(&r);
    date_differences(&r);
    date_time_differences(&r);
    time_differences(&r);
    additions(&r);
    instant_strings(&r);
    instant_epochs(&r);
    instant_rounding(&r);
    instant_text(&r);
    instant_arithmetic(&r);
    instant_comparisons(&r);
    date_strings(&r);
    date_utc(&r);
    date_getters(&r);
    sjs_vm_destroy(r.vm);
    sjs_context_destroy(r.context);
    if (live != 0) {
        fprintf(stderr, "temporal_test: %ld allocations left behind\n", live);
        failures++;
    }
    if (failures != 0) {
        fprintf(stderr, "temporal_test: %d failure(s)\n", failures);
        return 1;
    }
    if (!r.print) printf("temporal_test: %ld cases run, each compiled, deliberate faults fault, nothing leaks\n", r.cases);
    return 0;
}
