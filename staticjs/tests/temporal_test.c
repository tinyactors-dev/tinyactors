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
 * The cases, about 13,000, come from fixed lists, without randomness:
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
 *     mode, over times at each digit's halves, and at the limits.
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
                     * multiple even counted from midnight (the
                     * specification), @js-temporal/polyfill counted from the
                     * unit above, as the specification once did. They
                     * differ where the unit above holds an odd number of
                     * increments, below the hour, on the last tie of the
                     * day: that case is left out. */
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

int main(int argc, char **argv) {
    runner r = { 0 };
    r.print = argc == 2 && strcmp(argv[1], "--print") == 0;
    sjs_context_config config = { { NULL, &counted_vtable }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&r.context, &config) != SJS_OK) abort();
    if (sjs_vm_init(&r.vm, &(sjs_vm_config){ r.context, false }) != SJS_OK) abort();
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
