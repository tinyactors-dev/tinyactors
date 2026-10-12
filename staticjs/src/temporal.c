/*
 * Temporal values (docs/temporal.md): Temporal.PlainDate in the ISO 8601
 * calendar, its constructor, from, compare, getters and methods. The other
 * types (PlainTime, PlainDateTime, Duration, Instant) and Temporal.Now
 * build on the same kind, SJS_TEMPORAL, and the same tables.
 *
 * The rule: what JavaScript (temporal-polyfill 1.0.5, checked against
 * @js-temporal/polyfill 0.5.1) gives, or a fault. A RangeError or a
 * TypeError there is a fault here; so is a conversion of a string, an array
 * or an object where JavaScript would convert one to a number or a string.
 */
#include "vm.h"

#pragma STDC FP_CONTRACT OFF

/* ------------------------------------------------------------------------
 * The ISO 8601 calendar (the proleptic Gregorian one), in days since
 * 1970-01-01. Every division is of an int64 by a constant.
 */

/* Temporal's limits: a date between -271821-04-19 and +275760-09-13, one
 * day either side of the 10^8 days on each side of the epoch that an
 * Instant reaches. */
#define YEAR_MIN (-271821)
#define YEAR_MAX 275760
#define DAYS_MIN (-100000001)
#define DAYS_MAX 100000000

static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static int64_t floor_mod(int64_t a, int64_t b) {
    return a - floor_div(a, b) * b;
}

static bool leap_year(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static uint32_t days_in_month(int64_t year, uint32_t month) {
    static const uint8_t DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2u && leap_year(year) ? 29u : DAYS[month - 1u];
}

/* The days from 1970-01-01 to a date, negative before it. */
static int64_t epoch_days(int64_t year, uint32_t month, uint32_t day) {
    int64_t y = year - (month <= 2u ? 1 : 0);
    int64_t era = floor_div(y, 400);
    int64_t of_era = y - era * 400;
    int64_t shifted = month > 2u ? (int64_t)month - 3 : (int64_t)month + 9;
    int64_t of_year = (153 * shifted + 2) / 5 + (int64_t)day - 1;
    int64_t of_cycle = of_era * 365 + of_era / 4 - of_era / 100 + of_year;
    return era * 146097 + of_cycle - 719468;
}

/* 1 for Monday to 7 for Sunday; 1970-01-01 was a Thursday. */
static uint32_t day_of_week(int64_t days) {
    return (uint32_t)floor_mod(days + 3, 7) + 1u;
}

static uint32_t day_of_year(const sjs_iso_date_time *d) {
    return (uint32_t)(epoch_days(d->year, d->month, d->day) - epoch_days(d->year, 1u, 1u)) + 1u;
}

/* The ISO weeks of a year: 53 when it starts on a Thursday, or on a
 * Wednesday in a leap year. */
static uint32_t weeks_in_year(int64_t year) {
    uint32_t first = day_of_week(epoch_days(year, 1u, 1u));
    return first == 4u || (first == 3u && leap_year(year)) ? 53u : 52u;
}

/* The ISO week of a date and the year it belongs to: week 1 holds the
 * year's first Thursday. */
typedef struct {
    uint32_t week;
    int64_t year;
} iso_week;

static iso_week week_of(const sjs_iso_date_time *d) {
    uint32_t weekday = day_of_week(epoch_days(d->year, d->month, d->day));
    int64_t week = ((int64_t)day_of_year(d) - (int64_t)weekday + 10) / 7;
    if (week < 1) return (iso_week){ weeks_in_year((int64_t)d->year - 1), (int64_t)d->year - 1 };
    if (week > (int64_t)weeks_in_year(d->year)) return (iso_week){ 1u, (int64_t)d->year + 1 };
    return (iso_week){ (uint32_t)week, d->year };
}

/* Is a time of day past midnight? */
static bool after_midnight(const sjs_iso_date_time *d) {
    return (d->hour | d->minute | d->second | d->millisecond | d->microsecond | d->nanosecond) != 0;
}

/* Within Temporal's limits as a date (`with_time` false), or as a date and
 * time, whose least is a nanosecond after -271821-04-19T00:00. */
static bool within_limits(const sjs_iso_date_time *d, bool with_time) {
    if (d->year < YEAR_MIN || d->year > YEAR_MAX) return false;
    int64_t days = epoch_days(d->year, d->month, d->day);
    if (days > DAYS_MAX || days < DAYS_MIN) return false;
    return !(with_time && days == DAYS_MIN && !after_midnight(d));
}

/* -1, 0 or 1 as `a` is before, the same as or after `b`. */
static int compare_dates(const sjs_iso_date_time *a, const sjs_iso_date_time *b) {
    if (a->year != b->year) return a->year < b->year ? -1 : 1;
    if (a->month != b->month) return a->month < b->month ? -1 : 1;
    if (a->day != b->day) return a->day < b->day ? -1 : 1;
    return 0;
}

/* ------------------------------------------------------------------------
 * Text
 */

/* `value` in `width` digits or more, at `out`: their count. */
static uint32_t put_digits(char *out, uint64_t value, uint32_t width) {
    char room[24];
    uint32_t n = 0;
    do {
        room[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    while (n < width) room[n++] = '0';
    for (uint32_t i = 0; i < n; i++) out[i] = room[n - 1u - i];
    return n;
}

/* An ISO date: YYYY-MM-DD, or +YYYYYY-MM-DD and -YYYYYY-MM-DD outside the
 * years 0 to 9999. */
static uint32_t put_date(char *out, const sjs_iso_date_time *d) {
    uint32_t n = 0;
    if (d->year < 0 || d->year > 9999) {
        out[n++] = d->year < 0 ? '-' : '+';
        n += put_digits(out + n, (uint64_t)(d->year < 0 ? -(int64_t)d->year : (int64_t)d->year), 6u);
    } else {
        n += put_digits(out + n, (uint64_t)d->year, 4u);
    }
    out[n++] = '-';
    n += put_digits(out + n, d->month, 2u);
    out[n++] = '-';
    n += put_digits(out + n, d->day, 2u);
    return n;
}

uint32_t sjs_temporal_text(const sjs_temporal *t, char *out) {
    switch ((sjs_temporal_type)t->h.flags) {
    case SJS_TEMPORAL_PLAIN_DATE:
        return put_date(out, &t->as.iso);
    default:
        return 0;
    }
}

sjs_str sjs_temporal_type_name(sjs_temporal_type type) {
    switch (type) {
    case SJS_TEMPORAL_PLAIN_DATE:
        return SJS_STR("PlainDate");
    case SJS_TEMPORAL_PLAIN_TIME:
        return SJS_STR("PlainTime");
    case SJS_TEMPORAL_PLAIN_DATE_TIME:
        return SJS_STR("PlainDateTime");
    case SJS_TEMPORAL_INSTANT:
        return SJS_STR("Instant");
    default:
        return SJS_STR("Duration");
    }
}

/* Is `text` `lower` (lowercase ASCII), ASCII letters in either case? */
static bool equal_ignoring_case(sjs_str text, sjs_str lower) {
    if (text.length != lower.length) return false;
    for (uint32_t i = 0; i < text.length; i++) {
        char c = text.ptr[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower.ptr[i]) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------
 * ISO 8601 strings, as Temporal reads a date from one: what both
 * polyfills accept and give the same date for, the specification's grammar
 * (TemporalDateTimeString), and nothing else. A date, YYYY-MM-DD or
 * YYYYMMDD, with a sign and six digits for any year (not -000000); then
 * optionally a time after T, t or a space (HH, HH:MM, HH:MM:SS with a
 * fraction of 1 to 9 digits after . or ,, or the same without colons) and
 * a UTC offset (+HH to +HH:MM:SS.fffffffff, or without colons; never Z,
 * which a PlainDate refuses); then a time zone annotation, [Europe/Paris]
 * or [+01:00], ignored; then annotations, [u-ca=iso8601] or [key=value]:
 * the first u-ca must be iso8601 (in any case), a second one is refused
 * when any is critical (!), and an unknown key is ignored unless it is
 * critical. At most nine bracketed parts in all, as the polyfill reads.
 */

typedef struct {
    sjs_str text;
    uint32_t at;
} iso_scan;

static int scan_peek(const iso_scan *s) {
    return s->at < s->text.length ? (uint8_t)s->text.ptr[s->at] : -1;
}

static bool scan_take(iso_scan *s, char c) {
    if (scan_peek(s) != (uint8_t)c) return false;
    s->at++;
    return true;
}

static bool ascii_digit(int c) {
    return c >= '0' && c <= '9';
}

static bool ascii_letter(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Exactly `n` digits, read into `out`. */
static bool scan_digits(iso_scan *s, uint32_t n, uint32_t *out) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        int c = scan_peek(s);
        if (!ascii_digit(c)) return false;
        v = v * 10u + (uint32_t)(c - '0');
        s->at++;
    }
    *out = v;
    return true;
}

/* Two digits no greater than `max`. */
static bool scan_bounded(iso_scan *s, uint32_t max, uint32_t *out) {
    return scan_digits(s, 2u, out) && *out <= max;
}

/* A fraction after . or , of 1 to 9 digits, as nanoseconds; true with 0
 * when there is none. */
static bool scan_fraction(iso_scan *s, uint32_t *nanoseconds) {
    *nanoseconds = 0;
    if (!scan_take(s, '.') && !scan_take(s, ',')) return true;
    uint32_t n = 0;
    uint32_t v = 0;
    while (n < 9u && ascii_digit(scan_peek(s))) {
        v = v * 10u + (uint32_t)(scan_peek(s) - '0');
        s->at++;
        n++;
    }
    if (n == 0) return false;
    for (; n < 9u; n++) v *= 10u;
    *nanoseconds = v;
    return true;
}

/* HH, HH:MM, HH:MM:SS or HHMM, HHMMSS: the separator the same throughout.
 * `seconds` allows seconds (and a fraction after them); `second_max` is 60
 * for a time, which may be a leap second, and 59 for an offset. */
typedef struct {
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint32_t nanoseconds;
} clock_parts;

typedef struct {
    bool seconds;
    uint32_t second_max;
} clock_grammar;

static bool scan_clock(iso_scan *s, clock_grammar g, clock_parts *out) {
    *out = (clock_parts){ 0 };
    if (!scan_bounded(s, 23u, &out->hour)) return false;
    bool extended = scan_take(s, ':');
    if (!extended && !ascii_digit(scan_peek(s))) return true;
    if (!scan_bounded(s, 59u, &out->minute)) return false;
    if (!g.seconds) return true;
    if (extended ? !scan_take(s, ':') : !ascii_digit(scan_peek(s))) return true;
    if (!scan_bounded(s, g.second_max, &out->second)) return false;
    return scan_fraction(s, &out->nanoseconds);
}

/* A time zone's name: components of letters, digits, . _ - +, each led by
 * a letter, . or _, separated by /. Both polyfills read . and .. too. */
static bool time_zone_name(sjs_str name) {
    uint32_t start = 0;
    for (uint32_t i = 0; i <= name.length; i++) {
        if (i < name.length && name.ptr[i] != '/') {
            int c = (uint8_t)name.ptr[i];
            bool lead = ascii_letter(c) || c == '.' || c == '_';
            if (i == start ? !lead : !(lead || ascii_digit(c) || c == '-' || c == '+')) return false;
            continue;
        }
        if (i == start) return false;
        start = i + 1u;
    }
    return true;
}

/* A time zone annotation's identifier: an offset to the minute, or a name. */
static bool time_zone_identifier(sjs_str id) {
    if (id.length > 0 && (id.ptr[0] == '+' || id.ptr[0] == '-')) {
        iso_scan s = { id, 1u };
        clock_parts offset;
        return scan_clock(&s, (clock_grammar){ false, 59u }, &offset) && s.at == id.length;
    }
    return time_zone_name(id);
}

/* An annotation's key: a lowercase letter or _, then those, digits and -. */
static bool annotation_key(sjs_str key) {
    if (key.length == 0) return false;
    for (uint32_t i = 0; i < key.length; i++) {
        int c = (uint8_t)key.ptr[i];
        bool lead = (c >= 'a' && c <= 'z') || c == '_';
        if (i == 0 ? !lead : !(lead || ascii_digit(c) || c == '-')) return false;
    }
    return true;
}

/* An annotation's value: letters and digits, in parts joined by -. */
static bool annotation_value(sjs_str value) {
    if (value.length == 0 || value.ptr[0] == '-' || value.ptr[value.length - 1u] == '-') return false;
    for (uint32_t i = 0; i < value.length; i++) {
        int c = (uint8_t)value.ptr[i];
        if (c == '-' && value.ptr[i - 1u] == '-') return false;
        if (!(ascii_letter(c) || ascii_digit(c) || c == '-')) return false;
    }
    return true;
}

/* The bracketed parts after a date and time, to the end of the text. */
static bool scan_annotations(iso_scan *s) {
    uint32_t parts = 0;
    uint32_t calendars = 0;
    bool critical_calendar = false;
    while (scan_take(s, '[')) {
        if (++parts > 9u) return false;
        bool critical = scan_take(s, '!');
        uint32_t start = s->at;
        uint32_t equals = 0;
        while (s->at < s->text.length && s->text.ptr[s->at] != ']') {
            if (s->text.ptr[s->at] == '=' && equals == 0) equals = s->at;
            s->at++;
        }
        if (s->at == s->text.length) return false;
        sjs_str inside = { s->text.ptr + start, s->at - start };
        s->at++;
        if (equals == 0) {
            /* A time zone: only the first part. */
            if (parts != 1u || !time_zone_identifier(inside)) return false;
            continue;
        }
        sjs_str key = { s->text.ptr + start, equals - start };
        sjs_str value = { s->text.ptr + equals + 1u, s->at - 1u - (equals + 1u) };
        if (!annotation_key(key) || !annotation_value(value)) return false;
        if (sjs_str_equal(key, SJS_STR("u-ca"))) {
            if (++calendars == 1u && !equal_ignoring_case(value, SJS_STR("iso8601"))) return false;
            critical_calendar = critical_calendar || critical;
        } else if (critical) {
            return false;
        }
    }
    return s->at == s->text.length && !(calendars > 1u && critical_calendar);
}

/* A date, and a time when the text has one, from an ISO 8601 string. */
typedef struct {
    sjs_iso_date_time when;
    bool has_time;
} iso_parse;

static bool parse_iso(sjs_str text, iso_parse *out) {
    iso_scan s = { text, 0 };
    sjs_iso_date_time *d = &out->when;
    *out = (iso_parse){ 0 };
    uint32_t year;
    int sign = scan_peek(&s);
    if (sign == '+' || sign == '-') {
        s.at++;
        if (!scan_digits(&s, 6u, &year) || (sign == '-' && year == 0)) return false;
    } else if (!scan_digits(&s, 4u, &year)) {
        return false;
    }
    d->year = sign == '-' ? -(int32_t)year : (int32_t)year;
    bool extended = scan_take(&s, '-');
    uint32_t month;
    uint32_t day;
    if (!scan_digits(&s, 2u, &month) || (extended && !scan_take(&s, '-')) || !scan_digits(&s, 2u, &day)) return false;
    if (month < 1u || month > 12u || day < 1u || day > days_in_month(d->year, month)) return false;
    d->month = (uint8_t)month;
    d->day = (uint8_t)day;
    int separator = scan_peek(&s);
    if (separator == 'T' || separator == 't' || separator == ' ') {
        s.at++;
        clock_parts time;
        if (!scan_clock(&s, (clock_grammar){ true, 60u }, &time)) return false;
        out->has_time = true;
        d->hour = (uint8_t)time.hour;
        d->minute = (uint8_t)time.minute;
        d->second = (uint8_t)(time.second == 60u ? 59u : time.second);
        d->millisecond = (uint16_t)(time.nanoseconds / 1000000u);
        d->microsecond = (uint16_t)(time.nanoseconds / 1000u % 1000u);
        d->nanosecond = (uint16_t)(time.nanoseconds % 1000u);
        int c = scan_peek(&s);
        if (c == '+' || c == '-') {
            s.at++;
            clock_parts offset;
            if (!scan_clock(&s, (clock_grammar){ true, 59u }, &offset)) return false;
        }
    }
    return scan_annotations(&s) && within_limits(d, out->has_time);
}

/* ------------------------------------------------------------------------
 * Arguments and options
 */

/* A Temporal value of a fresh block, into `out`. */
static sjs_result temporal_copy(sjs_vm *vm, const sjs_temporal *from, sjs_value *out) {
    sjs_temporal *t;
    sjs_result r = sjs_temporal_make(vm->context, (sjs_temporal_type)from->h.flags, &t);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    t->as = from->as;
    *out = sjs_heap_value(SJS_TEMPORAL, &t->h);
    return SJS_OK;
}

static sjs_result new_date(sjs_vm *vm, const sjs_iso_date_time *d, sjs_value *out) {
    sjs_temporal t = { 0 };
    t.h.flags = SJS_TEMPORAL_PLAIN_DATE;
    t.as.iso = *d;
    return temporal_copy(vm, &t, out);
}

static sjs_value arg_of(const sjs_leaf_call *call, uint32_t i) {
    return i < call->count ? call->args[i] : sjs_undefined();
}

/* ToIntegerWithTruncation: a number, a boolean or null; NaN, an infinity
 * and undefined (NaN) throw a RangeError in JavaScript; a string, an array
 * or an object it would convert, which staticjs does not. */
static sjs_result integer_of(sjs_vm *vm, sjs_value v, double *out) {
    double d;
    if (sjs_vm_to_number(vm, v, &d) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!sjs_number_is_finite(d)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a Temporal field that is NaN, infinite or undefined");
    *out = sjs_number_trunc(d) + 0.0;
    return SJS_OK;
}

/* The option `name` of an options argument: undefined when there are no
 * options; an object's own property; undefined for any other object
 * (JavaScript finds none of Temporal's options on an array's or a map's
 * prototype); a fault for a primitive, which JavaScript refuses. */
typedef struct {
    sjs_value options;
    sjs_str name;
} option_name;

static sjs_result option_of(sjs_vm *vm, option_name option, sjs_value *out) {
    sjs_value options = option.options;
    *out = sjs_undefined();
    if (options.kind == SJS_UNDEFINED) return SJS_OK;
    if (sjs_is_primitive(options)) return sjs_vm_fault(vm, "JavaScript throws a TypeError for Temporal options that are not an object");
    if (options.kind != SJS_OBJECT) return SJS_OK;
    const sjs_entry *e = sjs_object_find(SJS_AS_OBJECT(options), option.name);
    if (e != NULL) *out = e->value;
    return SJS_OK;
}

/* A string option's value: one of `choices` (its index), left as it was
 * when it is undefined. */
typedef struct {
    sjs_value options;
    sjs_str name;
    const char *const *choices;
    uint32_t count;
} option_spec;

static sjs_result string_option(sjs_vm *vm, option_spec spec, uint32_t *out) {
    sjs_value v;
    if (option_of(vm, (option_name){ spec.options, spec.name }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    if (v.kind == SJS_UNDEFINED) return SJS_OK;
    if (v.kind != SJS_STRING) return sjs_vm_fault(vm, "staticjs reads a Temporal option only from a string, where JavaScript would convert this value to one");
    sjs_str text = sjs_string_view(SJS_AS_STRING(v));
    for (uint32_t i = 0; i < spec.count; i++) {
        if (sjs_str_equal(text, sjs_cstr(spec.choices[i]))) {
            *out = i;
            return SJS_OK;
        }
    }
    sjs_vm_fault(vm, "JavaScript throws a RangeError for this value of a Temporal option");
    sjs_error_subject(&vm->error, text);
    return SJS_ERR_PROGRAM;
}

/* overflow: "constrain" (the default) or "reject". */
static sjs_result overflow_rejects(sjs_vm *vm, sjs_value options, bool *reject) {
    static const char *const CHOICES[] = { "constrain", "reject" };
    uint32_t choice = 0;
    if (string_option(vm, (option_spec){ options, SJS_STR("overflow"), CHOICES, 2u }, &choice) != SJS_OK) return SJS_ERR_PROGRAM;
    *reject = choice == 1u;
    return SJS_OK;
}

/* A calendar given as a string: the ISO 8601 calendar's identifier, in any
 * case of its ASCII letters. JavaScript also reads another calendar, and
 * the calendar of an ISO string; staticjs has the ISO calendar only. */
static sjs_result iso_calendar(sjs_vm *vm, sjs_value v) {
    if (v.kind == SJS_STRING && equal_ignoring_case(sjs_string_view(SJS_AS_STRING(v)), SJS_STR("iso8601"))) return SJS_OK;
    return sjs_vm_fault(vm, "staticjs has only the ISO 8601 calendar, \"iso8601\"");
}

/* ------------------------------------------------------------------------
 * Property bags: { year, month, monthCode, day }
 */

typedef struct {
    double year;
    double month;
    double day;
    uint32_t month_code;   /* 1 to 12; 0 when there is none */
    bool has_year;
    bool has_month;
    bool has_day;
    bool any;              /* any of them, monthCode included */
    bool reject;           /* overflow "reject" */
} date_fields;

/* "M01" to "M12". JavaScript reads a monthCode from a string (an object
 * through its toString, which staticjs does not); "M13", "M00" or a leap
 * month "M01L" is a RangeError, in the ISO calendar. */
static sjs_result month_code_of(sjs_vm *vm, sjs_value v, uint32_t *out) {
    if (v.kind != SJS_STRING) return sjs_vm_fault(vm, "staticjs reads a monthCode only from a string");
    sjs_str text = sjs_string_view(SJS_AS_STRING(v));
    if (text.length == 3u && text.ptr[0] == 'M' && ascii_digit((uint8_t)text.ptr[1]) && ascii_digit((uint8_t)text.ptr[2])) {
        uint32_t month = (uint32_t)(text.ptr[1] - '0') * 10u + (uint32_t)(text.ptr[2] - '0');
        if (month >= 1u && month <= 12u) {
            *out = month;
            return SJS_OK;
        }
    }
    sjs_vm_fault(vm, "JavaScript throws a RangeError for a monthCode the ISO calendar does not have");
    sjs_error_subject(&vm->error, text);
    return SJS_ERR_PROGRAM;
}

/* The fields of a bag, each read when it is there and not undefined, and
 * converted as JavaScript converts it; `any` when any was there. */
static sjs_result read_date_fields(sjs_vm *vm, const sjs_object *bag, date_fields *f) {
    const sjs_entry *e = sjs_object_find(bag, SJS_STR("day"));
    if (e != NULL && e->value.kind != SJS_UNDEFINED) {
        if (integer_of(vm, e->value, &f->day) != SJS_OK) return SJS_ERR_PROGRAM;
        if (f->day < 1) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a day below 1");
        f->has_day = f->any = true;
    }
    e = sjs_object_find(bag, SJS_STR("month"));
    if (e != NULL && e->value.kind != SJS_UNDEFINED) {
        if (integer_of(vm, e->value, &f->month) != SJS_OK) return SJS_ERR_PROGRAM;
        if (f->month < 1) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a month below 1");
        f->has_month = f->any = true;
    }
    e = sjs_object_find(bag, SJS_STR("monthCode"));
    if (e != NULL && e->value.kind != SJS_UNDEFINED) {
        if (month_code_of(vm, e->value, &f->month_code) != SJS_OK) return SJS_ERR_PROGRAM;
        f->any = true;
    }
    e = sjs_object_find(bag, SJS_STR("year"));
    if (e != NULL && e->value.kind != SJS_UNDEFINED) {
        if (integer_of(vm, e->value, &f->year) != SJS_OK) return SJS_ERR_PROGRAM;
        f->has_year = f->any = true;
    }
    return SJS_OK;
}

/* A date of the fields: month and monthCode must agree; with "constrain",
 * a month past 12 is 12 and a day past the month's last is its last; with
 * "reject", either faults. Then within Temporal's limits. */
static sjs_result resolve_date(sjs_vm *vm, const date_fields *f, sjs_iso_date_time *out) {
    bool reject = f->reject;
    if (!f->has_year || !f->has_day || (!f->has_month && f->month_code == 0)) {
        return sjs_vm_fault(vm, "JavaScript throws a TypeError for a PlainDate's fields without a year, a month or monthCode, and a day");
    }
    if (f->has_month && f->month_code != 0 && f->month != (double)f->month_code) {
        return sjs_vm_fault(vm, "JavaScript throws a RangeError for a month and a monthCode that differ");
    }
    if (f->year < YEAR_MIN || f->year > YEAR_MAX) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a date outside Temporal's limits");
    double month = f->has_month ? f->month : (double)f->month_code;
    if (month > 12) {
        if (reject) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a month past 12 with overflow \"reject\"");
        month = 12;
    }
    *out = (sjs_iso_date_time){ 0 };
    out->year = (int32_t)f->year;
    out->month = (uint8_t)month;
    uint32_t last = days_in_month(out->year, out->month);
    double day = f->day;
    if (day > last) {
        if (reject) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a day past its month's last with overflow \"reject\"");
        day = last;
    }
    out->day = (uint8_t)day;
    if (!within_limits(out, false)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a date outside Temporal's limits");
    return SJS_OK;
}

/* ToTemporalDate: a PlainDate as it is, a string parsed, or a bag's
 * fields; with the options' overflow, which is read (and checked) for each. */
typedef struct {
    sjs_value item;
    sjs_value options;
} date_source;

static sjs_result to_date(sjs_vm *vm, date_source source, sjs_iso_date_time *out) {
    sjs_value item = source.item;
    sjs_value options = source.options;
    bool reject = false;
    if (item.kind == SJS_TEMPORAL && sjs_temporal_type_of(item) == SJS_TEMPORAL_PLAIN_DATE) {
        *out = SJS_AS_TEMPORAL(item)->as.iso;
        return overflow_rejects(vm, options, &reject);
    }
    if (item.kind == SJS_STRING) {
        sjs_str text = sjs_string_view(SJS_AS_STRING(item));
        vm->fuel_used += text.length / 16u;
        iso_parse parsed;
        if (!parse_iso(text, &parsed)) {
            sjs_vm_fault(vm, "staticjs reads a PlainDate from an ISO 8601 date in the ISO calendar within Temporal's limits, and this is not one");
            sjs_error_subject(&vm->error, text);
            return SJS_ERR_PROGRAM;
        }
        *out = parsed.when;
        out->hour = out->minute = out->second = 0;
        out->millisecond = out->microsecond = out->nanosecond = 0;
        return overflow_rejects(vm, options, &reject);
    }
    if (item.kind != SJS_OBJECT) {
        return sjs_vm_fault(vm, "a PlainDate is made from a PlainDate, an ISO 8601 string or an object of its fields");
    }
    const sjs_object *bag = SJS_AS_OBJECT(item);
    const sjs_entry *calendar = sjs_object_find(bag, SJS_STR("calendar"));
    if (calendar != NULL && calendar->value.kind != SJS_UNDEFINED && iso_calendar(vm, calendar->value) != SJS_OK) return SJS_ERR_PROGRAM;
    date_fields f = { 0 };
    if (read_date_fields(vm, bag, &f) != SJS_OK) return SJS_ERR_PROGRAM;
    if (overflow_rejects(vm, options, &f.reject) != SJS_OK) return SJS_ERR_PROGRAM;
    return resolve_date(vm, &f, out);
}

/* ------------------------------------------------------------------------
 * Functions: the constructor, from and compare
 */

/* new Temporal.PlainDate(year, month, day, calendar): every field as
 * ToIntegerWithTruncation gives it, the date valid as it is ("reject"). */
static sjs_result construct_date(sjs_vm *vm, sjs_leaf_call *call) {
    double fields[3];
    for (uint32_t i = 0; i < 3u; i++) {
        if (integer_of(vm, arg_of(call, i), &fields[i]) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    sjs_value calendar = arg_of(call, 3u);
    if (calendar.kind != SJS_UNDEFINED && iso_calendar(vm, calendar) != SJS_OK) return SJS_ERR_PROGRAM;
    if (fields[1] < 1 || fields[1] > 12) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a month outside 1 to 12");
    if (fields[0] < YEAR_MIN || fields[0] > YEAR_MAX) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a date outside Temporal's limits");
    sjs_iso_date_time d = { 0 };
    d.year = (int32_t)fields[0];
    d.month = (uint8_t)fields[1];
    if (fields[2] < 1 || fields[2] > days_in_month(d.year, d.month)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a day outside its month");
    d.day = (uint8_t)fields[2];
    if (!within_limits(&d, false)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a date outside Temporal's limits");
    return new_date(vm, &d, &call->result);
}

sjs_result sjs_temporal_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_iso_date_time a;
    switch (id) {
    case SJS_BUILTIN_TEMPORAL_PLAIN_DATE:
        return construct_date(vm, call);
    case SJS_BUILTIN_TEMPORAL_PLAIN_DATE_FROM: {
        sjs_value item = arg_of(call, 0);
        if (to_date(vm, (date_source){ item, arg_of(call, 1u) }, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        if (item.kind == SJS_TEMPORAL) {
            /* Equal, and immutable: the same value serves. */
            call->result = item;
            sjs_ref(item);
            return SJS_OK;
        }
        return new_date(vm, &a, &call->result);
    }
    default: {
        sjs_iso_date_time b;
        if (to_date(vm, (date_source){ arg_of(call, 0), sjs_undefined() }, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        if (to_date(vm, (date_source){ arg_of(call, 1u), sjs_undefined() }, &b) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_num((double)compare_dates(&a, &b));
        return SJS_OK;
    }
    }
}

/* ------------------------------------------------------------------------
 * Getters
 */

#define TYPE_BIT(type) (1u << (type))
#define PLAIN_DATE TYPE_BIT(SJS_TEMPORAL_PLAIN_DATE)
#define PLAIN_TIME TYPE_BIT(SJS_TEMPORAL_PLAIN_TIME)
#define PLAIN_DATE_TIME TYPE_BIT(SJS_TEMPORAL_PLAIN_DATE_TIME)
#define INSTANT TYPE_BIT(SJS_TEMPORAL_INSTANT)
#define DURATION TYPE_BIT(SJS_TEMPORAL_DURATION)
#define ALL_TYPES (PLAIN_DATE | PLAIN_TIME | PLAIN_DATE_TIME | INSTANT | DURATION)
#define DATED (PLAIN_DATE | PLAIN_DATE_TIME)
#define TIMED (PLAIN_TIME | PLAIN_DATE_TIME)

/* The types whose prototype has each getter (sjs_temporal_field). */
static const uint8_t FIELD_TYPES[SJS_FIELD_COUNT] = {
    [SJS_FIELD_BLANK] = DURATION,
    [SJS_FIELD_CALENDAR_ID] = DATED,
    [SJS_FIELD_DAY] = DATED,
    [SJS_FIELD_DAY_OF_WEEK] = DATED,
    [SJS_FIELD_DAY_OF_YEAR] = DATED,
    [SJS_FIELD_DAYS] = DURATION,
    [SJS_FIELD_DAYS_IN_MONTH] = DATED,
    [SJS_FIELD_DAYS_IN_WEEK] = DATED,
    [SJS_FIELD_DAYS_IN_YEAR] = DATED,
    [SJS_FIELD_EPOCH_MILLISECONDS] = INSTANT,
    [SJS_FIELD_EPOCH_NANOSECONDS] = INSTANT,
    [SJS_FIELD_ERA] = DATED,
    [SJS_FIELD_ERA_YEAR] = DATED,
    [SJS_FIELD_HOUR] = TIMED,
    [SJS_FIELD_HOURS] = DURATION,
    [SJS_FIELD_IN_LEAP_YEAR] = DATED,
    [SJS_FIELD_MICROSECOND] = TIMED,
    [SJS_FIELD_MICROSECONDS] = DURATION,
    [SJS_FIELD_MILLISECOND] = TIMED,
    [SJS_FIELD_MILLISECONDS] = DURATION,
    [SJS_FIELD_MINUTE] = TIMED,
    [SJS_FIELD_MINUTES] = DURATION,
    [SJS_FIELD_MONTH] = DATED,
    [SJS_FIELD_MONTH_CODE] = DATED,
    [SJS_FIELD_MONTHS] = DURATION,
    [SJS_FIELD_MONTHS_IN_YEAR] = DATED,
    [SJS_FIELD_NANOSECOND] = TIMED,
    [SJS_FIELD_NANOSECONDS] = DURATION,
    [SJS_FIELD_SECOND] = TIMED,
    [SJS_FIELD_SECONDS] = DURATION,
    [SJS_FIELD_SIGN] = DURATION,
    [SJS_FIELD_WEEK_OF_YEAR] = DATED,
    [SJS_FIELD_WEEKS] = DURATION,
    [SJS_FIELD_YEAR] = DATED,
    [SJS_FIELD_YEAR_OF_WEEK] = DATED,
    [SJS_FIELD_YEARS] = DURATION,
};

/* The types whose prototype has a method of each name, from
 * SJS_METHOD_TEMPORAL_ABS on. */
#define METHOD_INDEX(id) ((id) - SJS_METHOD_TEMPORAL_ABS)
static const uint8_t METHOD_TYPES[] = {
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_ABS)] = DURATION,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_ADD)] = ALL_TYPES,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_EQUALS)] = PLAIN_DATE | PLAIN_TIME | PLAIN_DATE_TIME | INSTANT,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_NEGATED)] = DURATION,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_ROUND)] = PLAIN_TIME | PLAIN_DATE_TIME | INSTANT | DURATION,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_SINCE)] = PLAIN_DATE | PLAIN_TIME | PLAIN_DATE_TIME | INSTANT,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_SUBTRACT)] = ALL_TYPES,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_JSON)] = ALL_TYPES,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_LOCALE_STRING)] = ALL_TYPES,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_DATE)] = PLAIN_DATE_TIME,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_DATE_TIME)] = PLAIN_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_MONTH_DAY)] = PLAIN_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_TIME)] = PLAIN_DATE_TIME,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_YEAR_MONTH)] = PLAIN_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_STRING)] = ALL_TYPES,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME)] = DATED,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME_ISO)] = INSTANT,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TOTAL)] = DURATION,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_UNTIL)] = PLAIN_DATE | PLAIN_TIME | PLAIN_DATE_TIME | INSTANT,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_VALUE_OF)] = ALL_TYPES,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_WITH)] = PLAIN_DATE | PLAIN_TIME | PLAIN_DATE_TIME | DURATION,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_WITH_CALENDAR)] = DATED,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_WITH_PLAIN_TIME)] = PLAIN_DATE_TIME,
};
_Static_assert(sizeof METHOD_TYPES == METHOD_INDEX(SJS_METHOD_TEMPORAL_WITH_PLAIN_TIME) + 1, "a Temporal method without its types");

/* The method of a name that a value's type has, or NONE. */
static sjs_builtin_id temporal_method_of(const sjs_name *n, sjs_value v) {
    if (n == NULL || n->methods[8] == 0) return SJS_BUILTIN_NONE;
    sjs_builtin_id id = (sjs_builtin_id)n->methods[8];
    return (METHOD_TYPES[METHOD_INDEX(id)] & TYPE_BIT(sjs_temporal_type_of(v))) != 0 ? id : SJS_BUILTIN_NONE;
}

/* The getter of a name that a value's type has, or NONE. */
static sjs_temporal_field temporal_field_of(const sjs_name *n, sjs_value v) {
    if (n == NULL || n->field == SJS_FIELD_NONE) return SJS_FIELD_NONE;
    return (FIELD_TYPES[n->field] & TYPE_BIT(sjs_temporal_type_of(v))) != 0 ? (sjs_temporal_field)n->field : SJS_FIELD_NONE;
}

static sjs_result temporal_string(sjs_vm *vm, sjs_str text, sjs_value *out) {
    sjs_string *s;
    sjs_result r = sjs_string_make(vm->context, text, &s);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    *out = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

/* A date's getter. */
typedef struct {
    const sjs_iso_date_time *date;
    sjs_temporal_field field;
} date_getter;

static sjs_result date_field(sjs_vm *vm, date_getter getter, sjs_value *out) {
    const sjs_iso_date_time *d = getter.date;
    switch (getter.field) {
    case SJS_FIELD_CALENDAR_ID:
        return temporal_string(vm, SJS_STR("iso8601"), out);
    case SJS_FIELD_YEAR:
        *out = sjs_num((double)d->year);
        return SJS_OK;
    case SJS_FIELD_MONTH:
        *out = sjs_num((double)d->month);
        return SJS_OK;
    case SJS_FIELD_MONTH_CODE: {
        char code[3] = { 'M', (char)('0' + d->month / 10u), (char)('0' + d->month % 10u) };
        return temporal_string(vm, (sjs_str){ code, 3u }, out);
    }
    case SJS_FIELD_DAY:
        *out = sjs_num((double)d->day);
        return SJS_OK;
    case SJS_FIELD_DAY_OF_WEEK:
        *out = sjs_num((double)day_of_week(epoch_days(d->year, d->month, d->day)));
        return SJS_OK;
    case SJS_FIELD_DAY_OF_YEAR:
        *out = sjs_num((double)day_of_year(d));
        return SJS_OK;
    case SJS_FIELD_WEEK_OF_YEAR:
        *out = sjs_num((double)week_of(d).week);
        return SJS_OK;
    case SJS_FIELD_YEAR_OF_WEEK:
        *out = sjs_num((double)week_of(d).year);
        return SJS_OK;
    case SJS_FIELD_DAYS_IN_WEEK:
        *out = sjs_num(7);
        return SJS_OK;
    case SJS_FIELD_DAYS_IN_MONTH:
        *out = sjs_num((double)days_in_month(d->year, d->month));
        return SJS_OK;
    case SJS_FIELD_DAYS_IN_YEAR:
        *out = sjs_num(leap_year(d->year) ? 366 : 365);
        return SJS_OK;
    case SJS_FIELD_MONTHS_IN_YEAR:
        *out = sjs_num(12);
        return SJS_OK;
    case SJS_FIELD_IN_LEAP_YEAR:
        *out = sjs_boolean(leap_year(d->year));
        return SJS_OK;
    default:
        /* era and eraYear: undefined in the ISO calendar. */
        *out = sjs_undefined();
        return SJS_OK;
    }
}

sjs_result sjs_temporal_property(sjs_vm *vm, sjs_member member, sjs_value *out) {
    sjs_value v = member.object;
    const sjs_name *n = sjs_name_of(member.key);
    *out = sjs_undefined();
    sjs_temporal_field field = temporal_field_of(n, v);
    if (field != SJS_FIELD_NONE) {
        if (sjs_temporal_type_of(v) != SJS_TEMPORAL_PLAIN_DATE) return sjs_vm_fault(vm, "staticjs does not build this Temporal type's getters yet");
        vm->fuel_used++;
        return date_field(vm, (date_getter){ &SJS_AS_TEMPORAL(v)->as.iso, field }, out);
    }
    if (temporal_method_of(n, v) != SJS_BUILTIN_NONE || (n != NULL && (n->flags & SJS_NAME_OBJECT) != 0)) {
        sjs_vm_fault(vm, "staticjs calls methods; it does not read them as values");
        sjs_error_subject(&vm->error, sjs_string_view(member.key));
        return SJS_ERR_PROGRAM;
    }
    return SJS_OK;
}

sjs_result sjs_temporal_callee(sjs_vm *vm, sjs_member member, sjs_value *out) {
    sjs_builtin_id id = temporal_method_of(sjs_name_of(member.key), member.object);
    if (id == SJS_BUILTIN_NONE) {
        sjs_vm_fault(vm, "staticjs does not have this method");
        sjs_error_subject(&vm->error, sjs_string_view(member.key));
        return SJS_ERR_PROGRAM;
    }
    *out = (sjs_value){ .kind = SJS_FUNCTION, .internal = SJS_INTERNAL_METHOD, .boolean = (uint32_t)id };
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Methods
 */

/* with(fields, options): the receiver's fields, replaced by those the bag
 * has (a month or a monthCode replaces both), resolved as from resolves
 * them. A bag with calendar or timeZone, or with none of the fields, is a
 * TypeError in JavaScript; so is a Temporal value or anything not an
 * object. */
static sjs_result date_with(sjs_vm *vm, const sjs_iso_date_time *d, sjs_leaf_call *call) {
    sjs_value item = arg_of(call, 0);
    if (item.kind != SJS_OBJECT) return sjs_vm_fault(vm, "with takes an object of the fields to change");
    const sjs_object *bag = SJS_AS_OBJECT(item);
    const sjs_entry *e = sjs_object_find(bag, SJS_STR("calendar"));
    const sjs_entry *zone = sjs_object_find(bag, SJS_STR("timeZone"));
    if ((e != NULL && e->value.kind != SJS_UNDEFINED) || (zone != NULL && zone->value.kind != SJS_UNDEFINED)) {
        return sjs_vm_fault(vm, "JavaScript throws a TypeError for with's fields with a calendar or a timeZone");
    }
    date_fields f = { 0 };
    if (read_date_fields(vm, bag, &f) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!f.any) return sjs_vm_fault(vm, "JavaScript throws a TypeError for with's fields without a year, month, monthCode or day");
    if (!f.has_year) {
        f.year = d->year;
        f.has_year = true;
    }
    if (!f.has_day) {
        f.day = d->day;
        f.has_day = true;
    }
    if (!f.has_month && f.month_code == 0) {
        f.month = d->month;
        f.has_month = true;
    }
    if (overflow_rejects(vm, arg_of(call, 1u), &f.reject) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_iso_date_time made;
    if (resolve_date(vm, &f, &made) != SJS_OK) return SJS_ERR_PROGRAM;
    return new_date(vm, &made, &call->result);
}

/* toString(options): calendarName "auto" and "never" write no calendar,
 * "always" writes [u-ca=iso8601] and "critical" [!u-ca=iso8601]. */
typedef struct {
    const sjs_temporal *value;
    sjs_value options;
} text_request;

static sjs_result date_to_string(sjs_vm *vm, text_request request, sjs_value *out) {
    static const char *const CHOICES[] = { "auto", "never", "always", "critical" };
    const sjs_temporal *t = request.value;
    uint32_t choice = 0;
    if (string_option(vm, (option_spec){ request.options, SJS_STR("calendarName"), CHOICES, 4u }, &choice) != SJS_OK) return SJS_ERR_PROGRAM;
    char text[SJS_TEMPORAL_TEXT_MAX + 16u];
    uint32_t n = sjs_temporal_text(t, text);
    sjs_str suffix = choice == 2u ? SJS_STR("[u-ca=iso8601]") : choice == 3u ? SJS_STR("[!u-ca=iso8601]") : SJS_STR("");
    sjs_copy(text + n, suffix.ptr, suffix.length);
    vm->fuel_used += n + suffix.length;
    return temporal_string(vm, (sjs_str){ text, n + suffix.length }, out);
}

sjs_result sjs_temporal_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    if (call->receiver.kind != SJS_TEMPORAL) return sjs_vm_fault(vm, "this method is a Temporal value's");
    const sjs_temporal *t = SJS_AS_TEMPORAL(call->receiver);
    if ((sjs_temporal_type)t->h.flags != SJS_TEMPORAL_PLAIN_DATE) return sjs_vm_fault(vm, "staticjs does not build this Temporal type's methods yet");
    switch (id) {
    case SJS_METHOD_TEMPORAL_EQUALS: {
        sjs_iso_date_time other;
        if (to_date(vm, (date_source){ arg_of(call, 0), sjs_undefined() }, &other) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_boolean(compare_dates(&t->as.iso, &other) == 0);
        return SJS_OK;
    }
    case SJS_METHOD_TEMPORAL_WITH:
        return date_with(vm, &t->as.iso, call);
    case SJS_METHOD_TEMPORAL_TO_STRING:
        return date_to_string(vm, (text_request){ t, arg_of(call, 0) }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_JSON:
        return date_to_string(vm, (text_request){ t, sjs_undefined() }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_LOCALE_STRING:
        return sjs_vm_fault(vm, "toLocaleString depends on locale data, which staticjs does not have");
    case SJS_METHOD_TEMPORAL_VALUE_OF:
        return sjs_vm_fault(vm, "JavaScript throws a TypeError for valueOf of a Temporal value: compare with equals or compare");
    case SJS_METHOD_TEMPORAL_ADD:
    case SJS_METHOD_TEMPORAL_SUBTRACT:
    case SJS_METHOD_TEMPORAL_UNTIL:
    case SJS_METHOD_TEMPORAL_SINCE:
        return sjs_vm_fault(vm, "staticjs does not build add, subtract, until and since of a PlainDate yet: they take or give a Temporal.Duration");
    case SJS_METHOD_TEMPORAL_TO_PLAIN_DATE_TIME:
        return sjs_vm_fault(vm, "staticjs does not build toPlainDateTime yet: it gives a Temporal.PlainDateTime");
    case SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME:
        return sjs_vm_fault(vm, "staticjs has no Temporal.ZonedDateTime: it needs time zone data");
    case SJS_METHOD_TEMPORAL_WITH_CALENDAR:
        return sjs_vm_fault(vm, "staticjs has only the ISO 8601 calendar, and no withCalendar");
    default:
        return sjs_vm_fault(vm, "staticjs does not have Temporal.PlainYearMonth or Temporal.PlainMonthDay");
    }
}
