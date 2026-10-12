/*
 * Date (docs/date.md): a time value, whole milliseconds since
 * 1970-01-01T00:00Z within 8.64e15 either way, or NaN for an invalid date,
 * held in a value of the Temporal kind (SJS_TEMPORAL_DATE, `as.time`). UTC
 * only: what reads or writes local time faults, as does what reads the
 * host's locale; the current time is the host clock's (sjs_vm_set_clock).
 *
 * The rule: what Node 22 (V8) gives, or a fault. Date.UTC follows V8's
 * MakeDay, MakeTime and MakeDate operation for operation, in doubles where
 * V8 computes in doubles, so that every value is V8's bit for bit.
 */
#include "vm.h"

#pragma STDC FP_CONTRACT OFF

#define DATE_MS_PER_DAY INT64_C(86400000)
#define DATE_TIME_LIMIT 8.64e15

static double date_nan(void) {
    return 0.0 / 0.0;
}

/* TimeClip: NaN past 8.64e15 either way, else the number truncated, -0
 * made +0. */
static double date_clip(double t) {
    if (!(t >= -DATE_TIME_LIMIT && t <= DATE_TIME_LIMIT)) return date_nan();
    return sjs_number_trunc(t) + 0.0;
}

static int64_t date_floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static bool date_leap_year(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static uint32_t date_days_in_month(int64_t year, uint32_t month) {
    static const uint8_t DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2u && date_leap_year(year) ? 29u : DAYS[month - 1u];
}

/* The days from 1970-01-01 to a date (month 1 to 12), negative before it. */
static int64_t date_days_from(int64_t year, uint32_t month, uint32_t day) {
    int64_t y = year - (month <= 2u ? 1 : 0);
    int64_t era = date_floor_div(y, 400);
    int64_t of_era = y - era * 400;
    int64_t shifted = month > 2u ? (int64_t)month - 3 : (int64_t)month + 9;
    int64_t of_year = (153 * shifted + 2) / 5 + (int64_t)day - 1;
    return era * 146097 + of_era * 365 + of_era / 4 - of_era / 100 + of_year - 719468;
}

/* A valid time value's UTC fields, as the getUTC methods give them. */
typedef struct {
    int64_t year;
    uint32_t month;       /* 0 to 11 */
    uint32_t day;         /* 1 to 31 */
    uint32_t weekday;     /* 0 for Sunday to 6 */
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint32_t millisecond;
} utc_fields;

static utc_fields utc_fields_of(double time) {
    utc_fields f;
    int64_t t = (int64_t)time;
    int64_t days = date_floor_div(t, DATE_MS_PER_DAY);
    int64_t ms = t - days * DATE_MS_PER_DAY;
    int64_t z = days + 719468;
    int64_t era = date_floor_div(z, 146097);
    int64_t of_era = z - era * 146097;
    int64_t year_of_era = (of_era - of_era / 1460 + of_era / 36524 - of_era / 146096) / 365;
    int64_t of_year = of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    int64_t shifted = (5 * of_year + 2) / 153;
    int64_t month = shifted < 10 ? shifted + 3 : shifted - 9;
    f.year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    f.month = (uint32_t)(month - 1);
    f.day = (uint32_t)(of_year - (153 * shifted + 2) / 5 + 1);
    f.weekday = (uint32_t)(days + 4 - date_floor_div(days + 4, 7) * 7);
    f.hour = (uint32_t)(ms / 3600000);
    f.minute = (uint32_t)(ms / 60000 % 60);
    f.second = (uint32_t)(ms / 1000 % 60);
    f.millisecond = (uint32_t)(ms % 1000);
    return f;
}

/* ------------------------------------------------------------------------
 * Text
 */

static uint32_t date_put_digits(char *out, uint64_t value, uint32_t width) {
    char room[20];
    uint32_t n = 0;
    do {
        room[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    uint32_t written = 0;
    while (n + written < width) out[written++] = '0';
    while (n > 0) out[written++] = room[--n];
    return written;
}

static uint32_t date_put_text(char *out, const char *text) {
    uint32_t n = 0;
    while (text[n] != '\0') {
        out[n] = text[n];
        n++;
    }
    return n;
}

/* HH:mm:ss, of toISOString and toUTCString. */
static uint32_t date_put_clock(char *out, const utc_fields *f) {
    uint32_t n = date_put_digits(out, f->hour, 2u);
    out[n++] = ':';
    n += date_put_digits(out + n, f->minute, 2u);
    out[n++] = ':';
    n += date_put_digits(out + n, f->second, 2u);
    return n;
}

/* toISOString: YYYY-MM-DDTHH:mm:ss.sssZ, a year outside 0 to 9999 as a
 * sign and six digits. */
uint32_t sjs_date_iso_text(double time, char *out) {
    if (sjs_number_is_nan(time)) return 0;
    utc_fields f = utc_fields_of(time);
    uint32_t n = 0;
    if (f.year >= 0 && f.year <= 9999) {
        n += date_put_digits(out, (uint64_t)f.year, 4u);
    } else {
        out[n++] = f.year < 0 ? '-' : '+';
        n += date_put_digits(out + n, (uint64_t)(f.year < 0 ? -f.year : f.year), 6u);
    }
    out[n++] = '-';
    n += date_put_digits(out + n, f.month + 1u, 2u);
    out[n++] = '-';
    n += date_put_digits(out + n, f.day, 2u);
    out[n++] = 'T';
    n += date_put_clock(out + n, &f);
    out[n++] = '.';
    n += date_put_digits(out + n, f.millisecond, 3u);
    out[n++] = 'Z';
    return n;
}

/* toUTCString, as V8 writes it: "Tue, 30 Jan 2024 00:00:00 GMT", the year
 * in at least four digits after its sign ("-0001"). */
static uint32_t date_utc_text(double time, char *out) {
    static const char *const WEEKDAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    utc_fields f = utc_fields_of(time);
    uint32_t n = date_put_text(out, WEEKDAYS[f.weekday]);
    n += date_put_text(out + n, ", ");
    n += date_put_digits(out + n, f.day, 2u);
    out[n++] = ' ';
    n += date_put_text(out + n, MONTHS[f.month]);
    out[n++] = ' ';
    if (f.year < 0) out[n++] = '-';
    n += date_put_digits(out + n, (uint64_t)(f.year < 0 ? -f.year : f.year), 4u);
    out[n++] = ' ';
    n += date_put_clock(out + n, &f);
    n += date_put_text(out + n, " GMT");
    return n;
}

/* ------------------------------------------------------------------------
 * The date time string format (ECMAScript's Date Time String Format):
 * YYYY, YYYY-MM or YYYY-MM-DD (a year of a sign and six digits too, not
 * -000000), alone (UTC), or then THH:mm, THH:mm:ss or THH:mm:ss.sss and Z or
 * an offset +HH:mm or -HH:mm. A date and time without Z or an offset is
 * local time in JavaScript, which staticjs does not have: it faults. Every
 * other string faults too: JavaScript reads it by heuristics of its own
 * (Node reads "Jan 1 2024", "2024/01/01" and "2024-1-1"; and a day past its
 * month's end, which it moves into the next month). Only a string of the
 * format whose time is past the limits gives NaN, as in JavaScript.
 */

typedef enum { DATE_READ, DATE_LOCAL_TIME, DATE_UNREAD } date_reading;

typedef struct {
    sjs_str text;
    uint32_t at;
} date_scan;

static bool date_take(date_scan *s, char c) {
    if (s->at < s->text.length && s->text.ptr[s->at] == c) {
        s->at++;
        return true;
    }
    return false;
}

/* Exactly `count` digits, into `out`. */
static bool date_digits(date_scan *s, uint32_t count, int64_t *out) {
    int64_t value = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (s->at >= s->text.length) return false;
        char c = s->text.ptr[s->at];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        s->at++;
    }
    *out = value;
    return true;
}

static bool date_at_end(const date_scan *s) {
    return s->at == s->text.length;
}

/* The date of the string, YYYY[-MM[-DD]], into days since the epoch. */
static bool date_scan_date(date_scan *s, int64_t *days) {
    int64_t year;
    int64_t month = 1;
    int64_t day = 1;
    bool negative = date_take(s, '-');
    if (negative || date_take(s, '+')) {
        if (!date_digits(s, 6u, &year) || (negative && year == 0)) return false;
        if (negative) year = -year;
    } else if (!date_digits(s, 4u, &year)) {
        return false;
    }
    if (date_take(s, '-')) {
        if (!date_digits(s, 2u, &month) || month < 1 || month > 12) return false;
        if (date_take(s, '-')) {
            if (!date_digits(s, 2u, &day) || day < 1 || day > (int64_t)date_days_in_month(year, (uint32_t)month)) return false;
        }
    }
    *days = date_days_from(year, (uint32_t)month, (uint32_t)day);
    return true;
}

/* THH:mm[:ss[.sss]] (24:00, 24:00:00 and 24:00:00.000 the next midnight),
 * into milliseconds. */
static bool date_scan_time(date_scan *s, int64_t *ms) {
    int64_t hour, minute;
    int64_t second = 0;
    int64_t millisecond = 0;
    if (!date_take(s, 'T') || !date_digits(s, 2u, &hour) || !date_take(s, ':') || !date_digits(s, 2u, &minute)) return false;
    if (date_take(s, ':')) {
        if (!date_digits(s, 2u, &second)) return false;
        if (date_take(s, '.') && !date_digits(s, 3u, &millisecond)) return false;
    }
    if (hour > 24 || minute > 59 || second > 59) return false;
    if (hour == 24 && (minute != 0 || second != 0 || millisecond != 0)) return false;
    *ms = ((hour * 60 + minute) * 60 + second) * 1000 + millisecond;
    return true;
}

static date_reading date_parse(sjs_str text, double *out) {
    date_scan s = { text, 0 };
    int64_t days;
    int64_t ms = 0;
    if (!date_scan_date(&s, &days)) return DATE_UNREAD;
    if (!date_at_end(&s)) {
        if (!date_scan_time(&s, &ms)) return DATE_UNREAD;
        if (date_at_end(&s)) return DATE_LOCAL_TIME;
        if (!date_take(&s, 'Z')) {
            bool negative = date_take(&s, '-');
            int64_t hours, minutes;
            if (!negative && !date_take(&s, '+')) return DATE_UNREAD;
            if (!date_digits(&s, 2u, &hours) || !date_take(&s, ':') || !date_digits(&s, 2u, &minutes)) return DATE_UNREAD;
            if (hours > 23 || minutes > 59) return DATE_UNREAD;
            int64_t offset = (hours * 60 + minutes) * 60000;
            ms -= negative ? -offset : offset;
        }
        if (!date_at_end(&s)) return DATE_UNREAD;
    }
    /* Within an int64: the days of a six-digit year are below 4 x 10^8. */
    int64_t t = days * DATE_MS_PER_DAY + ms;
    *out = (t >= -INT64_C(8640000000000000) && t <= INT64_C(8640000000000000)) ? (double)t : date_nan();
    return DATE_READ;
}

/* A string's time value, or a fault. */
static sjs_result date_of_string(sjs_vm *vm, sjs_value v, double *out) {
    sjs_str text = sjs_string_view(SJS_AS_STRING(v));
    vm->fuel_used += text.length / 16u;
    date_reading reading = date_parse(text, out);
    if (reading == DATE_READ) return SJS_OK;
    sjs_vm_fault(vm, reading == DATE_LOCAL_TIME
                     ? "a date and time without Z or an offset is local time in JavaScript, which staticjs does not have: add Z"
                     : "staticjs reads only the date time string format, as 2024-01-31 or 2024-01-31T12:30:00.000Z; JavaScript reads other strings by heuristics of its own");
    sjs_error_subject(&vm->error, text);
    return SJS_ERR_PROGRAM;
}

/* ------------------------------------------------------------------------
 * Date.UTC: V8's MakeDay, MakeTime and MakeDate (src/date/date.cc).
 */

/* MakeDay: a year within a million and a month within ten million either
 * way, truncated, the month carried into the year; then the days to the
 * month's first, counted in int arithmetic as V8 counts them (which
 * truncates toward zero where a year past the limits makes its count
 * negative), plus the date truncated. */
static double date_make_day(double year, double month, double date) {
    if (!(year >= -1000000.0 && year <= 1000000.0 && month >= -10000000.0 && month <= 10000000.0 && sjs_number_is_finite(date))) {
        return date_nan();
    }
    static const int64_t FROM_MONTH[2][12] = {
        { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 },
        { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 },
    };
    const int64_t delta = 399999;
    const int64_t base = 365 * (1970 + delta) + (1970 + delta) / 4 - (1970 + delta) / 100 + (1970 + delta) / 400;
    int64_t y = (int64_t)year;
    int64_t m = (int64_t)month;
    y += m / 12;
    m %= 12;
    if (m < 0) {
        m += 12;
        y -= 1;
    }
    int64_t from_year = 365 * (y + delta) + (y + delta) / 4 - (y + delta) / 100 + (y + delta) / 400 - base;
    bool common = (y % 4 != 0) || (y % 100 == 0 && y % 400 != 0);
    from_year += FROM_MONTH[common ? 0 : 1][m];
    return (double)(from_year - 1) + sjs_number_trunc(date);
}

/* MakeTime, in doubles: h * 3600000 + m * 60000 + s * 1000 + ms. */
static double date_make_time(const double parts[4]) {
    for (uint32_t i = 0; i < 4u; i++) {
        if (!sjs_number_is_finite(parts[i])) return date_nan();
    }
    double h = sjs_number_trunc(parts[0]);
    double m = sjs_number_trunc(parts[1]);
    double s = sjs_number_trunc(parts[2]);
    double ms = sjs_number_trunc(parts[3]);
    return h * 3600000.0 + m * 60000.0 + s * 1000.0 + ms;
}

static double date_make_date(double day, double time) {
    if (!sjs_number_is_finite(day) || !sjs_number_is_finite(time)) return date_nan();
    if (time == 0.0 && day != 0.0) return day * 86400000.0;
    return time + day * 86400000.0;
}

/* Date.UTC(year, month, date, hours, minutes, seconds, ms): each argument
 * ToNumber (a string or an object faults: JavaScript converts it), the
 * year NaN without one, the month 0, the date 1 and the rest 0; a year 0 to
 * 99, truncated, is 1900 plus it. */
static sjs_result date_utc(sjs_vm *vm, sjs_leaf_call *call) {
    double v[7] = { date_nan(), 0, 1, 0, 0, 0, 0 };
    for (uint32_t i = 0; i < 7u && i < call->count; i++) {
        if (sjs_vm_to_number(vm, call->args[i], &v[i]) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    if (!sjs_number_is_nan(v[0])) {
        double y = sjs_number_trunc(v[0]);
        if (0.0 <= y && y <= 99.0) v[0] = 1900.0 + y;
    }
    double day = date_make_day(v[0], v[1], v[2]);
    double time = date_make_time(&v[3]);
    call->result = sjs_num(date_clip(date_make_date(day, time)));
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Functions and methods
 */

/* The host clock (docs/temporal.md, "Host clock") in milliseconds, toward
 * negative infinity: its seconds are, and its nanoseconds are past them. */
static sjs_result date_clock(sjs_vm *vm, double *out) {
    if (!vm->clock_set) return sjs_vm_fault(vm, "the host has not set the clock (sjs_vm_set_clock)");
    *out = (double)(vm->clock.seconds * 1000 + (int64_t)(vm->clock.nanoseconds / 1000000u));
    return SJS_OK;
}

static sjs_result date_make(sjs_vm *vm, double time, sjs_value *out) {
    sjs_temporal *t;
    sjs_result r = sjs_temporal_make(vm->context, SJS_TEMPORAL_DATE, &t);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    t->as.time = time;
    *out = sjs_heap_value(SJS_TEMPORAL, &t->h);
    return SJS_OK;
}

static sjs_result date_string(sjs_vm *vm, sjs_str text, sjs_value *out) {
    sjs_string *s;
    sjs_result r = sjs_string_make(vm->context, text, &s);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    *out = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

/* new Date(): the clock; new Date(value): a Date's time value, a string
 * read as Date.parse reads it, or TimeClip of ToNumber of anything else
 * (an object faults: JavaScript converts it). Two or more arguments are a
 * date and time in local time: a fault. */
static sjs_result date_construct(sjs_vm *vm, sjs_leaf_call *call) {
    double time;
    if (call->count == 0) {
        if (date_clock(vm, &time) != SJS_OK) return SJS_ERR_PROGRAM;
        return date_make(vm, time, &call->result);
    }
    if (call->count > 1u) {
        return sjs_vm_fault(vm, "new Date(year, month, ...) reads its fields in local time, which staticjs does not have: write new Date(Date.UTC(year, month, ...))");
    }
    sjs_value v = call->args[0];
    if (v.kind == SJS_TEMPORAL && sjs_temporal_type_of(v) == SJS_TEMPORAL_DATE) {
        time = SJS_AS_TEMPORAL(v)->as.time;
    } else if (v.kind == SJS_STRING) {
        if (date_of_string(vm, v, &time) != SJS_OK) return SJS_ERR_PROGRAM;
    } else {
        if (sjs_vm_to_number(vm, v, &time) != SJS_OK) return SJS_ERR_PROGRAM;
        time = date_clip(time);
    }
    return date_make(vm, time, &call->result);
}

sjs_result sjs_date_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    double time;
    switch (id) {
    case SJS_BUILTIN_DATE:
        return date_construct(vm, call);
    case SJS_BUILTIN_DATE_NOW:
        if (date_clock(vm, &time) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_num(time);
        return SJS_OK;
    case SJS_BUILTIN_DATE_PARSE: {
        sjs_value v = call->count > 0 ? call->args[0] : sjs_undefined();
        if (v.kind != SJS_STRING) return sjs_vm_fault(vm, "Date.parse reads a string here: JavaScript would turn this value into one first");
        if (date_of_string(vm, v, &time) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_num(time);
        return SJS_OK;
    }
    case SJS_BUILTIN_DATE_UTC:
        return date_utc(vm, call);
    default:
        return sjs_vm_fault(vm, "Date() gives the date and time in local time, which staticjs does not have: write new Date().toISOString()");
    }
}

sjs_result sjs_date_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    double time = SJS_AS_TEMPORAL(call->receiver)->as.time;
    char text[SJS_TEMPORAL_TEXT_MAX];
    switch (id) {
    case SJS_METHOD_TEMPORAL_DATE_GET_TIME:
    case SJS_METHOD_TEMPORAL_VALUE_OF:
        call->result = sjs_num(time);
        return SJS_OK;
    case SJS_METHOD_TEMPORAL_DATE_TO_ISO_STRING:
        if (sjs_number_is_nan(time)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for toISOString of an invalid Date");
        return date_string(vm, (sjs_str){ text, sjs_date_iso_text(time, text) }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_JSON:
        /* Its key argument is not read. */
        if (sjs_number_is_nan(time)) {
            call->result = sjs_null();
            return SJS_OK;
        }
        return date_string(vm, (sjs_str){ text, sjs_date_iso_text(time, text) }, &call->result);
    case SJS_METHOD_TEMPORAL_DATE_TO_UTC_STRING:
        if (sjs_number_is_nan(time)) return date_string(vm, SJS_STR("Invalid Date"), &call->result);
        return date_string(vm, (sjs_str){ text, date_utc_text(time, text) }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_LOCALE_STRING:
        return sjs_vm_fault(vm, "toLocaleString depends on locale data, which staticjs does not have");
    case SJS_METHOD_TEMPORAL_TO_STRING:
    case SJS_METHOD_TEMPORAL_DATE_LOCAL:
        /* MUTATION: the message's setUTC methods. Restore "...: use
         * toISOString or the getUTC methods". */
        return sjs_vm_fault(vm, "this method of a Date reads local time, which staticjs does not have: use toISOString, the getUTC methods or the setUTC methods");
    case SJS_METHOD_TEMPORAL_DATE_SETTER:
        /* MUTATION: the message. Restore "a Date's setters change it, and
         * staticjs does not have them yet". */
        return sjs_vm_fault(vm, "a Date's setters change it: staticjs calls them only on a variable of the function that holds the Date alone");
    default:
        break;
    }
    if (sjs_number_is_nan(time)) {
        call->result = sjs_num(date_nan());
        return SJS_OK;
    }
    utc_fields f = utc_fields_of(time);
    double value;
    switch (id) {
    case SJS_METHOD_TEMPORAL_DATE_GET_UTC_FULL_YEAR:
        value = (double)f.year;
        break;
    case SJS_METHOD_TEMPORAL_DATE_GET_UTC_MONTH:
        value = f.month;
        break;
    case SJS_METHOD_TEMPORAL_DATE_GET_UTC_DATE:
        value = f.day;
        break;
    case SJS_METHOD_TEMPORAL_DATE_GET_UTC_DAY:
        value = f.weekday;
        break;
    case SJS_METHOD_TEMPORAL_DATE_GET_UTC_HOURS:
        value = f.hour;
        break;
    case SJS_METHOD_TEMPORAL_DATE_GET_UTC_MINUTES:
        value = f.minute;
        break;
    case SJS_METHOD_TEMPORAL_DATE_GET_UTC_SECONDS:
        value = f.second;
        break;
    default: /* GET_UTC_MILLISECONDS */
        value = f.millisecond;
        break;
    }
    call->result = sjs_num(value);
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * MUTATION: a Date's setTime and UTC setters (docs/date.md, "Setters"), on a
 * variable of the function that holds the Date alone (SJS_OP_MUTATE). They
 * follow V8's builtins (src/builtins/builtins-date.cc): each argument
 * ToNumber (a string or an object faults: JavaScript converts it; one left
 * out keeps the Date's field, an undefined one is NaN); the fields of an
 * invalid date are not read, and the setter gives NaN, but setUTCFullYear,
 * which starts from +0; then MakeDay, MakeTime, MakeDate and TimeClip, as
 * Date.UTC. The new time value is written in place and is the result. This
 * section exists only for mutation.
 */

/* The setters' fields, in MakeDay's and MakeTime's order. */
enum { DATE_YEAR, DATE_MONTH, DATE_DAY, DATE_HOUR, DATE_MINUTE, DATE_SECOND, DATE_MILLISECOND, DATE_FIELDS };

/* The first field each setter sets, after setTime: setUTCFullYear sets the
 * year and on, ..., setUTCMilliseconds the millisecond alone. */
static uint32_t date_set_first(sjs_builtin_id id) {
    static const uint8_t FIRST[7] = { DATE_YEAR, DATE_MONTH, DATE_DAY, DATE_HOUR, DATE_MINUTE, DATE_SECOND, DATE_MILLISECOND };
    return FIRST[id - SJS_METHOD_DATE_SET_UTC_FULL_YEAR];
}

/* The fields a setter sets at most: setUTCFullYear the date's three,
 * setUTCMonth two, setUTCDate one; setUTCHours the time's four, ... */
static uint32_t date_set_last(uint32_t first) {
    return first <= DATE_DAY ? DATE_DAY : DATE_MILLISECOND;
}

sjs_result sjs_date_set(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_temporal *date = SJS_AS_TEMPORAL(*call->receiver_slot);
    double time = date->as.time;
    if (id == SJS_METHOD_DATE_SET_TIME) {
        double v = date_nan();
        if (call->count > 0 && sjs_vm_to_number(vm, call->args[0], &v) != SJS_OK) return SJS_ERR_PROGRAM;
        date->as.time = date_clip(v);
        call->result = sjs_num(date->as.time);
        return SJS_OK;
    }
    /* The fields it sets: from its first, one per argument given (the first
     * always, as JavaScript reads it given or not: undefined is NaN), to
     * its last at most; the arguments are converted first. */
    uint32_t first = date_set_first(id);
    uint32_t given = call->count < 1u ? 1u : call->count;
    uint32_t last = first + given - 1u < date_set_last(first) ? first + given - 1u : date_set_last(first);
    double v[DATE_FIELDS];
    for (uint32_t i = first; i <= last; i++) {
        sjs_value a = i - first < call->count ? call->args[i - first] : sjs_undefined();
        if (sjs_vm_to_number(vm, a, &v[i]) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    if (sjs_number_is_nan(time)) {
        if (id != SJS_METHOD_DATE_SET_UTC_FULL_YEAR) {
            call->result = sjs_num(time);
            return SJS_OK;
        }
        time = 0.0;
    }
    /* The fields it keeps: the Date's own. */
    utc_fields f = utc_fields_of(time);
    double kept[DATE_FIELDS] = { (double)f.year, f.month, f.day, f.hour, f.minute, f.second, f.millisecond };
    for (uint32_t i = 0; i < DATE_FIELDS; i++) {
        if (i < first || i > last) v[i] = kept[i];
    }
    int64_t t = (int64_t)time;
    int64_t days = date_floor_div(t, DATE_MS_PER_DAY);
    double day = (double)days;
    double within = (double)(t - days * DATE_MS_PER_DAY);
    if (first <= DATE_DAY) {
        day = date_make_day(v[DATE_YEAR], v[DATE_MONTH], v[DATE_DAY]);
    } else {
        within = date_make_time(&v[DATE_HOUR]);
    }
    date->as.time = date_clip(date_make_date(day, within));
    call->result = sjs_num(date->as.time);
    return SJS_OK;
}
