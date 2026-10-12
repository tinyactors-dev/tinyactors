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
 * The cases, about 3,000, come from fixed lists, without randomness:
 *   - ISO 8601 strings for Temporal.PlainDate.from: dates, times, offsets and
 *     annotations, each list varied with the others held;
 *   - dates' getters (day of week and of year, ISO weeks, leap years) over
 *     years at the edges of the calendar's cycles and of Temporal's limits,
 *     on the days around the ends of their years;
 *   - Temporal's limits through the constructor, a bag and a string;
 *   - property bags with every overflow, month, monthCode and day edge, for
 *     from and for with;
 *   - toString's calendarName.
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

/* Temporal.PlainDate.from of the parts joined; each string once. */
typedef struct {
    text seen;   /* every string so far, each followed by a NUL */
} strings;

static bool seen_before(strings *s, const text *string) {
    for (size_t at = 0; at < s->seen.length; at += strlen(s->seen.ptr + at) + 1) {
        if (strcmp(s->seen.ptr + at, string->ptr) == 0) return true;
    }
    put_bytes(&s->seen, string->ptr, string->length + 1);
    return false;
}

static void from_string(runner *r, strings *s, const part *parts[4]) {
    text string = { 0 };
    put(&string, "");
    bool refused = false;
    for (int i = 0; i < 4; i++) {
        put(&string, parts[i]->text);
        refused = refused || parts[i]->refused;
    }
    if (!seen_before(s, &string)) {
        text source = { 0 };
        put(&source, "Temporal.PlainDate.from(");
        put_quoted(&source, string.ptr, string.length);
        put(&source, ")");
        evaluate(r, &source, refused);
        free(source.ptr);
    }
    free(string.ptr);
}

static void iso_strings(runner *r) {
    static const part none = { "", 0 }, at_ten = { "T10:00", 0 }, plus_one = { "+01:00", 0 }, zulu = { "Z", 0 }, utc = { "[UTC]", 0 };
    static const part edges[] = { { "2024-01-31", 0 }, { "-271821-04-19", 0 }, { "+275760-09-13", 0 } };
    static const part offset_times[] = { { "T10:00", 0 }, { "T10", 0 }, { "T1000", 0 }, { "T10:00:00.5", 0 } };
    static const part bases[] = { { "2024-01-31", 0 }, { "2024-01-31T10:00", 0 }, { "2024-01-31T10:00+01:00", 0 }, { "20240131T1000-0100", 0 } };
    strings s = { { 0 } };
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

int main(int argc, char **argv) {
    runner r = { 0 };
    r.print = argc == 2 && strcmp(argv[1], "--print") == 0;
    sjs_context_config config = { { NULL, &counted_vtable }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&r.context, &config) != SJS_OK) abort();
    if (sjs_vm_init(&r.vm, &(sjs_vm_config){ r.context, false }) != SJS_OK) abort();
    iso_strings(&r);
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
