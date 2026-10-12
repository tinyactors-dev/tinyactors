/*
 * Temporal values (docs/temporal.md): Temporal.PlainDate, PlainTime and
 * PlainDateTime in the ISO 8601 calendar, their constructors, from,
 * compare, getters and methods. The other types (Duration, Instant) and
 * Temporal.Now build on the same kind, SJS_TEMPORAL, and the same tables.
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

/* The date `days` after 1970-01-01 (before it when negative), into `d`'s
 * date fields: epoch_days undone. */
static void date_of_days(int64_t days, sjs_iso_date_time *d) {
    int64_t z = days + 719468;
    int64_t era = floor_div(z, 146097);
    int64_t of_era = z - era * 146097;
    int64_t year_of_era = (of_era - of_era / 1460 + of_era / 36524 - of_era / 146096) / 365;
    int64_t of_year = of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    int64_t shifted = (5 * of_year + 2) / 153;
    int64_t month = shifted < 10 ? shifted + 3 : shifted - 9;
    d->year = (int32_t)(year_of_era + era * 400 + (month <= 2 ? 1 : 0));
    d->month = (uint8_t)month;
    d->day = (uint8_t)(of_year - (153 * shifted + 2) / 5 + 1);
}

/* A time of day in nanoseconds since midnight: below 86,400 * 10^9, which
 * an int64 holds with room to spare. */
#define NS_PER_DAY INT64_C(86400000000000)

static int64_t time_of_day(const sjs_iso_date_time *d) {
    int64_t seconds = ((int64_t)d->hour * 60 + d->minute) * 60 + d->second;
    return seconds * 1000000000 + (int64_t)d->millisecond * 1000000 + (int64_t)d->microsecond * 1000 + d->nanosecond;
}

/* `d`'s time fields from nanoseconds since midnight, 0 to a day less 1. */
static void set_time_of_day(sjs_iso_date_time *d, int64_t ns) {
    d->nanosecond = (uint16_t)(ns % 1000);
    d->microsecond = (uint16_t)(ns / 1000 % 1000);
    d->millisecond = (uint16_t)(ns / 1000000 % 1000);
    int64_t seconds = ns / 1000000000;
    d->second = (uint8_t)(seconds % 60);
    d->minute = (uint8_t)(seconds / 60 % 60);
    d->hour = (uint8_t)(seconds / 3600);
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

static int compare_times(const sjs_iso_date_time *a, const sjs_iso_date_time *b) {
    int64_t x = time_of_day(a);
    int64_t y = time_of_day(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

static int compare_date_times(const sjs_iso_date_time *a, const sjs_iso_date_time *b) {
    int order = compare_dates(a, b);
    return order != 0 ? order : compare_times(a, b);
}

/* ------------------------------------------------------------------------
 * Rounding: an integer of nanoseconds to a multiple of an increment, by
 * each of Temporal's rounding modes. The modes are relative to the sign:
 * expand rounds away from zero, trunc toward it, and their half modes
 * likewise on a tie; ceil and floor toward the infinities.
 */

typedef enum {
    MODE_CEIL,
    MODE_FLOOR,
    MODE_EXPAND,
    MODE_TRUNC,
    MODE_HALF_CEIL,
    MODE_HALF_FLOOR,
    MODE_HALF_EXPAND,
    MODE_HALF_TRUNC,
    MODE_HALF_EVEN
} rounding_mode;

static const char *const ROUNDING_MODES[] = { "ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven" };

typedef struct {
    int64_t increment;   /* nanoseconds, 1 to a day */
    rounding_mode mode;
} rounding;

static int64_t round_to(int64_t x, rounding r) {
    int64_t quotient = floor_div(x, r.increment);
    int64_t lower = quotient * r.increment;
    int64_t rest = x - lower;
    if (rest == 0) return x;
    bool negative = x < 0;
    bool up;
    switch (r.mode) {
    case MODE_CEIL:
        up = true;
        break;
    case MODE_FLOOR:
        up = false;
        break;
    case MODE_EXPAND:
        up = !negative;
        break;
    case MODE_TRUNC:
        up = negative;
        break;
    default:
        if (rest * 2 != r.increment) {
            up = rest * 2 > r.increment;
        } else if (r.mode == MODE_HALF_CEIL) {
            up = true;
        } else if (r.mode == MODE_HALF_FLOOR) {
            up = false;
        } else if (r.mode == MODE_HALF_EXPAND) {
            up = !negative;
        } else if (r.mode == MODE_HALF_TRUNC) {
            up = negative;
        } else {
            up = quotient % 2 != 0;   /* halfEven: to the even multiple */
        }
        break;
    }
    return up ? lower + r.increment : lower;
}

/* The units of Temporal's unit options, singular, then plural. */
typedef enum {
    UNIT_NANOSECOND,
    UNIT_MICROSECOND,
    UNIT_MILLISECOND,
    UNIT_SECOND,
    UNIT_MINUTE,
    UNIT_HOUR,
    UNIT_DAY,
    UNIT_WEEK,
    UNIT_MONTH,
    UNIT_YEAR,
    UNIT_COUNT
} time_unit;

static const char *const UNITS[] = {
    "nanosecond", "microsecond", "millisecond", "second", "minute", "hour", "day", "week", "month", "year",
    "nanoseconds", "microseconds", "milliseconds", "seconds", "minutes", "hours", "days", "weeks", "months", "years",
};

static const int64_t UNIT_NANOSECONDS[] = { 1, 1000, 1000000, 1000000000, INT64_C(60000000000), INT64_C(3600000000000), NS_PER_DAY };

/* Round a date and time, or a time, to `r`: the time of day rounds; a
 * PlainDateTime's (`dated`) carries a day into its date, which must stay
 * within Temporal's limits (else false), and a PlainTime's wraps to
 * midnight. */
static bool round_iso(sjs_iso_date_time *d, rounding r, bool dated) {
    int64_t ns = round_to(time_of_day(d), r);
    if (ns == NS_PER_DAY) {
        ns = 0;
        if (dated) date_of_days(epoch_days(d->year, d->month, d->day) + 1, d);
    }
    set_time_of_day(d, ns);
    return !dated || within_limits(d, true);
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

/* The precision of a time's text: to the minute (HH:MM), the seconds with
 * as many fraction digits as they need ("auto"), or 0 to 9 of them. */
#define DIGITS_MINUTE (-2)
#define DIGITS_AUTO (-1)

/* A time of day: HH:MM, then :SS and a fraction as `digits` says. */
static uint32_t put_time(char *out, const sjs_iso_date_time *d, int32_t digits) {
    uint32_t n = put_digits(out, d->hour, 2u);
    out[n++] = ':';
    n += put_digits(out + n, d->minute, 2u);
    if (digits == DIGITS_MINUTE) return n;
    out[n++] = ':';
    n += put_digits(out + n, d->second, 2u);
    char fraction[9];
    put_digits(fraction, (uint64_t)d->millisecond * 1000000u + (uint64_t)d->microsecond * 1000u + d->nanosecond, 9u);
    uint32_t count = (uint32_t)digits;
    if (digits == DIGITS_AUTO) {
        count = 9u;
        while (count > 0 && fraction[count - 1u] == '0') count--;
    }
    if (count == 0) return n;
    out[n++] = '.';
    for (uint32_t i = 0; i < count; i++) out[n++] = fraction[i];
    return n;
}

/* A plain value's text: its date, its time, or both joined by T. */
typedef struct {
    sjs_temporal_type type;
    const sjs_iso_date_time *iso;
} iso_value;

static uint32_t put_iso(char *out, iso_value v, int32_t digits) {
    uint32_t n = 0;
    if (v.type != SJS_TEMPORAL_PLAIN_TIME) n += put_date(out, v.iso);
    if (v.type == SJS_TEMPORAL_PLAIN_DATE_TIME) out[n++] = 'T';
    if (v.type != SJS_TEMPORAL_PLAIN_DATE) n += put_time(out + n, v.iso, digits);
    return n;
}

uint32_t sjs_temporal_text(const sjs_temporal *t, char *out) {
    switch ((sjs_temporal_type)t->h.flags) {
    case SJS_TEMPORAL_PLAIN_DATE:
    case SJS_TEMPORAL_PLAIN_TIME:
    case SJS_TEMPORAL_PLAIN_DATE_TIME:
        return put_iso(out, (iso_value){ (sjs_temporal_type)t->h.flags, &t->as.iso }, DIGITS_AUTO);
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
 * ISO 8601 strings, as Temporal reads a date, a time or both from one: what
 * both polyfills accept and give the same value for, the specification's
 * grammar (TemporalDateTimeString, TemporalTimeString), and nothing else.
 * A date, YYYY-MM-DD or
 * YYYYMMDD, with a sign and six digits for any year (not -000000); then
 * optionally a time after T, t or a space (HH, HH:MM, HH:MM:SS with a
 * fraction of 1 to 9 digits after . or ,, or the same without colons) and
 * a UTC offset (+HH to +HH:MM:SS.fffffffff, or without colons; never Z,
 * which a PlainDate refuses); then a time zone annotation, [Europe/Paris]
 * or [+01:00], ignored; then annotations, [u-ca=iso8601] or [key=value]:
 * the first u-ca must be iso8601 (in any case), a second one is refused
 * when any is critical (!), and an unknown key is ignored unless it is
 * critical. At most nine bracketed parts in all, as the polyfill reads.
 * A time may also stand alone (parse_time_only).
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

/* A clock's fields into a time's; a leap second, 60, is 59. */
static void set_clock(sjs_iso_date_time *d, const clock_parts *time) {
    d->hour = (uint8_t)time->hour;
    d->minute = (uint8_t)time->minute;
    d->second = (uint8_t)(time->second == 60u ? 59u : time->second);
    d->millisecond = (uint16_t)(time->nanoseconds / 1000000u);
    d->microsecond = (uint16_t)(time->nanoseconds / 1000u % 1000u);
    d->nanosecond = (uint16_t)(time->nanoseconds % 1000u);
}

/* A UTC offset after a time, if there is one: read and ignored. */
static bool scan_offset(iso_scan *s) {
    int c = scan_peek(s);
    if (c != '+' && c != '-') return true;
    s->at++;
    clock_parts offset;
    return scan_clock(s, (clock_grammar){ true, 59u }, &offset);
}

/* A date, and a time when the text has one, from an ISO 8601 string;
 * whether it is within Temporal's limits is the caller's to check. */
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
        set_clock(d, &time);
        if (!scan_offset(&s)) return false;
    }
    return scan_annotations(&s);
}

/* Is a time's text (with its offset) also a year and month, YYYY-MM or
 * YYYYMM, or a month and day, MM-DD or MMDD, of a day some year has? The
 * grammar refuses such a time without its T (TemporalTimeString), as
 * "2021-12" would be December 2021 or 20:21 at UTC-12. */
static bool reads_as_date(sjs_str text) {
    uint32_t n = text.length;
    bool dash = n == 5u || n == 7u;
    if ((n < 4u || n > 7u) || (dash && text.ptr[n - 3u] != '-')) return false;
    for (uint32_t i = 0; i < n; i++) {
        if (!(ascii_digit((uint8_t)text.ptr[i]) || (dash && i == n - 3u))) return false;
    }
    uint32_t first = (uint32_t)(text.ptr[0] - '0') * 10u + (uint32_t)(text.ptr[1] - '0');
    uint32_t last = (uint32_t)(text.ptr[n - 2u] - '0') * 10u + (uint32_t)(text.ptr[n - 1u] - '0');
    if (n >= 6u) return last >= 1u && last <= 12u;
    return first >= 1u && first <= 12u && last >= 1u && last <= days_in_month(2000, first);
}

/* A time alone: T or t, then the clock, an offset and annotations as
 * after a date; without the T, a time that reads as a date is refused. */
static bool parse_time_only(sjs_str text, sjs_iso_date_time *d) {
    iso_scan s = { text, 0 };
    bool designated = scan_take(&s, 'T') || scan_take(&s, 't');
    uint32_t start = s.at;
    clock_parts time;
    if (!scan_clock(&s, (clock_grammar){ true, 60u }, &time) || !scan_offset(&s)) return false;
    sjs_str clock = { text.ptr + start, s.at - start };
    if (!scan_annotations(&s) || (!designated && reads_as_date(clock))) return false;
    set_clock(d, &time);
    return true;
}

/* The time of an ISO string: a date and time's, or a time's alone. A date
 * without a time is not one. */
static bool parse_time(sjs_str text, sjs_iso_date_time *out) {
    iso_parse parsed;
    *out = (sjs_iso_date_time){ 0 };
    if (parse_iso(text, &parsed)) {
        set_time_of_day(out, time_of_day(&parsed.when));
        return parsed.has_time;
    }
    return parse_time_only(text, out);
}

/* ------------------------------------------------------------------------
 * Arguments and options
 */

/* A plain value of `v.type` holding `v.iso`'s fields of that type (a
 * PlainDate's time and a PlainTime's date are zero), into `out`. */
static sjs_result new_iso(sjs_vm *vm, iso_value v, sjs_value *out) {
    sjs_temporal *t;
    sjs_result r = sjs_temporal_make(vm->context, v.type, &t);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    t->as.iso = *v.iso;
    if (v.type == SJS_TEMPORAL_PLAIN_DATE) set_time_of_day(&t->as.iso, 0);
    if (v.type == SJS_TEMPORAL_PLAIN_TIME) {
        t->as.iso.year = 0;
        t->as.iso.month = t->as.iso.day = 0;
    }
    *out = sjs_heap_value(SJS_TEMPORAL, &t->h);
    return SJS_OK;
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
    if (!sjs_number_is_finite(d)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a Temporal field or option that is NaN, infinite or undefined");
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

/* The index of `text` among `count` choices, or `count`. */
static uint32_t choice_of(sjs_str text, const char *const *choices, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (sjs_str_equal(text, sjs_cstr(choices[i]))) return i;
    }
    return count;
}

static sjs_result option_choice_fault(sjs_vm *vm, sjs_str text) {
    sjs_vm_fault(vm, "JavaScript throws a RangeError for this value of a Temporal option");
    sjs_error_subject(&vm->error, text);
    return SJS_ERR_PROGRAM;
}

static sjs_result string_option(sjs_vm *vm, option_spec spec, uint32_t *out) {
    sjs_value v;
    if (option_of(vm, (option_name){ spec.options, spec.name }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    if (v.kind == SJS_UNDEFINED) return SJS_OK;
    if (v.kind != SJS_STRING) return sjs_vm_fault(vm, "staticjs reads a Temporal option only from a string, where JavaScript would convert this value to one");
    sjs_str text = sjs_string_view(SJS_AS_STRING(v));
    uint32_t i = choice_of(text, spec.choices, spec.count);
    if (i == spec.count) return option_choice_fault(vm, text);
    *out = i;
    return SJS_OK;
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

/* roundingMode, `*out` left as it is when it is undefined. */
static sjs_result mode_option(sjs_vm *vm, sjs_value options, rounding_mode *out) {
    uint32_t choice = (uint32_t)*out;
    if (string_option(vm, (option_spec){ options, SJS_STR("roundingMode"), ROUNDING_MODES, 9u }, &choice) != SJS_OK) return SJS_ERR_PROGRAM;
    *out = (rounding_mode)choice;
    return SJS_OK;
}

/* A unit's name, singular or plural, from a string. */
static sjs_result unit_of(sjs_vm *vm, sjs_str text, time_unit *out) {
    uint32_t i = choice_of(text, UNITS, 2u * UNIT_COUNT);
    if (i == 2u * UNIT_COUNT) return option_choice_fault(vm, text);
    *out = (time_unit)(i % UNIT_COUNT);
    return SJS_OK;
}

/* smallestUnit: a unit from `least` to `most`; `*given` false when it is
 * undefined. */
typedef struct {
    time_unit least;
    time_unit most;
} unit_range;

typedef struct {
    sjs_str text;
    unit_range range;
} unit_text;

typedef struct {
    sjs_value options;
    unit_range range;
} unit_option;

typedef struct {
    time_unit unit;
    bool given;
} unit_choice;

static sjs_result unit_in_range(sjs_vm *vm, unit_text unit, time_unit *out) {
    if (unit_of(vm, unit.text, out) != SJS_OK) return SJS_ERR_PROGRAM;
    if (*out < unit.range.least || *out > unit.range.most) {
        sjs_vm_fault(vm, "JavaScript throws a RangeError for a smallestUnit this method does not round to");
        sjs_error_subject(&vm->error, unit.text);
        return SJS_ERR_PROGRAM;
    }
    return SJS_OK;
}

static sjs_result smallest_unit(sjs_vm *vm, unit_option option, unit_choice *out) {
    sjs_value v;
    out->given = false;
    if (option_of(vm, (option_name){ option.options, SJS_STR("smallestUnit") }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    if (v.kind == SJS_UNDEFINED) return SJS_OK;
    if (v.kind != SJS_STRING) return sjs_vm_fault(vm, "staticjs reads a Temporal option only from a string, where JavaScript would convert this value to one");
    out->given = true;
    return unit_in_range(vm, (unit_text){ sjs_string_view(SJS_AS_STRING(v)), option.range }, &out->unit);
}

/* roundingIncrement: an integer from 1 to 10^9, 1 when undefined; then
 * one that divides the unit above `unit` into equal parts, fewer than one
 * (a day: exactly one). */
static sjs_result increment_option(sjs_vm *vm, sjs_value options, int64_t *out) {
    sjs_value v;
    *out = 1;
    if (option_of(vm, (option_name){ options, SJS_STR("roundingIncrement") }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    if (v.kind == SJS_UNDEFINED) return SJS_OK;
    double d;
    if (integer_of(vm, v, &d) != SJS_OK) return SJS_ERR_PROGRAM;
    if (d < 1 || d > 1e9) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a roundingIncrement below 1 or above 10^9");
    *out = (int64_t)d;
    return SJS_OK;
}

static sjs_result check_increment(sjs_vm *vm, int64_t increment, time_unit unit) {
    int64_t parts = unit == UNIT_DAY ? 1 : unit == UNIT_HOUR ? 24 : unit >= UNIT_SECOND ? 60 : 1000;
    bool fits = unit == UNIT_DAY ? increment == 1 : increment < parts && parts % increment == 0;
    if (!fits) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a roundingIncrement that does not divide the next larger unit");
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Property bags: { year, month, monthCode, day } and { hour, minute,
 * second, millisecond, microsecond, nanosecond }
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
 * "reject", either faults. Then within Temporal's limits as a date. */
static sjs_result resolve_date(sjs_vm *vm, const date_fields *f, sjs_iso_date_time *out) {
    bool reject = f->reject;
    if (!f->has_year || !f->has_day || (!f->has_month && f->month_code == 0)) {
        return sjs_vm_fault(vm, "JavaScript throws a TypeError for a date's fields without a year, a month or monthCode, and a day");
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

/* A bag's date fields, those it lacks taken from `d` (a month or a
 * monthCode replaces both), as with() takes them. */
static void merge_date_fields(date_fields *f, const sjs_iso_date_time *d) {
    if (!f->has_year) {
        f->year = d->year;
        f->has_year = true;
    }
    if (!f->has_day) {
        f->day = d->day;
        f->has_day = true;
    }
    if (!f->has_month && f->month_code == 0) {
        f->month = d->month;
        f->has_month = true;
    }
}

/* The time fields, hour to nanosecond, and each one's greatest value. */
#define TIME_FIELD_COUNT 6u
static const char *const TIME_FIELDS[TIME_FIELD_COUNT] = { "hour", "minute", "second", "millisecond", "microsecond", "nanosecond" };
static const double TIME_FIELD_MAX[TIME_FIELD_COUNT] = { 23, 59, 59, 999, 999, 999 };

typedef struct {
    double value[TIME_FIELD_COUNT];
    bool has[TIME_FIELD_COUNT];
    bool any;
    bool reject;   /* overflow "reject" */
} time_fields;

static sjs_result read_time_fields(sjs_vm *vm, const sjs_object *bag, time_fields *f) {
    for (uint32_t i = 0; i < TIME_FIELD_COUNT; i++) {
        const sjs_entry *e = sjs_object_find(bag, sjs_cstr(TIME_FIELDS[i]));
        if (e == NULL || e->value.kind == SJS_UNDEFINED) continue;
        if (integer_of(vm, e->value, &f->value[i]) != SJS_OK) return SJS_ERR_PROGRAM;
        f->has[i] = f->any = true;
    }
    return SJS_OK;
}

/* A time of the fields, those missing taken from `out`: with "constrain"
 * each is brought into its range; with "reject" one outside it faults. */
static sjs_result resolve_time(sjs_vm *vm, const time_fields *f, sjs_iso_date_time *out) {
    double value[TIME_FIELD_COUNT] = { out->hour, out->minute, out->second, out->millisecond, out->microsecond, out->nanosecond };
    for (uint32_t i = 0; i < TIME_FIELD_COUNT; i++) {
        if (!f->has[i]) continue;
        double v = f->value[i];
        if ((v < 0 || v > TIME_FIELD_MAX[i]) && f->reject) {
            sjs_vm_fault(vm, "JavaScript throws a RangeError for a time field outside its range, with overflow \"reject\" or in a constructor");
            sjs_error_subject(&vm->error, sjs_cstr(TIME_FIELDS[i]));
            return SJS_ERR_PROGRAM;
        }
        value[i] = v < 0 ? 0 : v > TIME_FIELD_MAX[i] ? TIME_FIELD_MAX[i] : v;
    }
    out->hour = (uint8_t)value[0];
    out->minute = (uint8_t)value[1];
    out->second = (uint8_t)value[2];
    out->millisecond = (uint16_t)value[3];
    out->microsecond = (uint16_t)value[4];
    out->nanosecond = (uint16_t)value[5];
    return SJS_OK;
}

/* A bag of with()'s fields: an object, not a Temporal value, without a
 * calendar or a timeZone (a TypeError in JavaScript). */
static sjs_result with_bag(sjs_vm *vm, sjs_value item, const sjs_object **out) {
    if (item.kind != SJS_OBJECT) return sjs_vm_fault(vm, "with takes an object of the fields to change");
    const sjs_object *bag = SJS_AS_OBJECT(item);
    const sjs_entry *e = sjs_object_find(bag, SJS_STR("calendar"));
    const sjs_entry *zone = sjs_object_find(bag, SJS_STR("timeZone"));
    if ((e != NULL && e->value.kind != SJS_UNDEFINED) || (zone != NULL && zone->value.kind != SJS_UNDEFINED)) {
        return sjs_vm_fault(vm, "JavaScript throws a TypeError for with's fields with a calendar or a timeZone");
    }
    *out = bag;
    return SJS_OK;
}

/* A string's text, charged for its length. */
static sjs_str parsed_text(sjs_vm *vm, sjs_value item) {
    sjs_str text = sjs_string_view(SJS_AS_STRING(item));
    vm->fuel_used += text.length / 16u;
    return text;
}

static sjs_result unparsed(sjs_vm *vm, const char *message, sjs_str text) {
    sjs_vm_fault(vm, message);
    sjs_error_subject(&vm->error, text);
    return SJS_ERR_PROGRAM;
}

static bool is_temporal(sjs_value v, sjs_temporal_type type) {
    return v.kind == SJS_TEMPORAL && sjs_temporal_type_of(v) == type;
}

/* ToTemporalDate: a PlainDate as it is, a PlainDateTime's date, a string
 * parsed, or a bag's fields; with the options' overflow, which is read
 * (and checked) for each. */
typedef struct {
    sjs_value item;
    sjs_value options;
} date_source;

static sjs_result to_date(sjs_vm *vm, date_source source, sjs_iso_date_time *out) {
    sjs_value item = source.item;
    sjs_value options = source.options;
    bool reject = false;
    *out = (sjs_iso_date_time){ 0 };
    if (is_temporal(item, SJS_TEMPORAL_PLAIN_DATE) || is_temporal(item, SJS_TEMPORAL_PLAIN_DATE_TIME)) {
        *out = SJS_AS_TEMPORAL(item)->as.iso;
        set_time_of_day(out, 0);
        return overflow_rejects(vm, options, &reject);
    }
    if (item.kind == SJS_STRING) {
        sjs_str text = parsed_text(vm, item);
        iso_parse parsed;
        if (!parse_iso(text, &parsed) || !within_limits(&parsed.when, parsed.has_time)) {
            return unparsed(vm, "staticjs reads a PlainDate from an ISO 8601 date in the ISO calendar within Temporal's limits, and this is not one", text);
        }
        *out = parsed.when;
        set_time_of_day(out, 0);
        return overflow_rejects(vm, options, &reject);
    }
    if (item.kind != SJS_OBJECT) {
        return sjs_vm_fault(vm, "a PlainDate is made from a PlainDate, a PlainDateTime, an ISO 8601 string or an object of its fields");
    }
    const sjs_object *bag = SJS_AS_OBJECT(item);
    const sjs_entry *calendar = sjs_object_find(bag, SJS_STR("calendar"));
    if (calendar != NULL && calendar->value.kind != SJS_UNDEFINED && iso_calendar(vm, calendar->value) != SJS_OK) return SJS_ERR_PROGRAM;
    date_fields f = { 0 };
    if (read_date_fields(vm, bag, &f) != SJS_OK) return SJS_ERR_PROGRAM;
    if (overflow_rejects(vm, options, &f.reject) != SJS_OK) return SJS_ERR_PROGRAM;
    return resolve_date(vm, &f, out);
}

/* ToTemporalTime: a PlainTime as it is, a PlainDateTime's time, a string
 * parsed, or a bag with at least one of the time fields; the options'
 * overflow read for each. */
static sjs_result to_time(sjs_vm *vm, date_source source, sjs_iso_date_time *out) {
    sjs_value item = source.item;
    bool reject = false;
    *out = (sjs_iso_date_time){ 0 };
    if (is_temporal(item, SJS_TEMPORAL_PLAIN_TIME) || is_temporal(item, SJS_TEMPORAL_PLAIN_DATE_TIME)) {
        set_time_of_day(out, time_of_day(&SJS_AS_TEMPORAL(item)->as.iso));
        return overflow_rejects(vm, source.options, &reject);
    }
    if (item.kind == SJS_STRING) {
        sjs_str text = parsed_text(vm, item);
        if (!parse_time(text, out)) {
            return unparsed(vm, "staticjs reads a PlainTime from an ISO 8601 time, or a date and time, and this is not one", text);
        }
        return overflow_rejects(vm, source.options, &reject);
    }
    if (item.kind != SJS_OBJECT) {
        return sjs_vm_fault(vm, "a PlainTime is made from a PlainTime, a PlainDateTime, an ISO 8601 string or an object of its fields");
    }
    time_fields f = { 0 };
    if (read_time_fields(vm, SJS_AS_OBJECT(item), &f) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!f.any) return sjs_vm_fault(vm, "JavaScript throws a TypeError for a time's fields without an hour, minute, second, millisecond, microsecond or nanosecond");
    if (overflow_rejects(vm, source.options, &f.reject) != SJS_OK) return SJS_ERR_PROGRAM;
    return resolve_time(vm, &f, out);
}

static sjs_result outside_limits(sjs_vm *vm) {
    return sjs_vm_fault(vm, "JavaScript throws a RangeError for a date outside Temporal's limits");
}

/* ToTemporalDateTime: a PlainDateTime as it is, a PlainDate at midnight, a
 * string parsed, or a bag of a date's fields and a time's; the options'
 * overflow read for each, and the result within Temporal's limits. */
static sjs_result to_date_time(sjs_vm *vm, date_source source, sjs_iso_date_time *out) {
    sjs_value item = source.item;
    bool reject = false;
    *out = (sjs_iso_date_time){ 0 };
    if (is_temporal(item, SJS_TEMPORAL_PLAIN_DATE_TIME) || is_temporal(item, SJS_TEMPORAL_PLAIN_DATE)) {
        *out = SJS_AS_TEMPORAL(item)->as.iso;
        if (overflow_rejects(vm, source.options, &reject) != SJS_OK) return SJS_ERR_PROGRAM;
    } else if (item.kind == SJS_STRING) {
        sjs_str text = parsed_text(vm, item);
        iso_parse parsed;
        if (!parse_iso(text, &parsed)) {
            return unparsed(vm, "staticjs reads a PlainDateTime from an ISO 8601 date, or a date and time, in the ISO calendar, and this is not one", text);
        }
        *out = parsed.when;
        if (overflow_rejects(vm, source.options, &reject) != SJS_OK) return SJS_ERR_PROGRAM;
    } else if (item.kind == SJS_OBJECT) {
        const sjs_object *bag = SJS_AS_OBJECT(item);
        const sjs_entry *calendar = sjs_object_find(bag, SJS_STR("calendar"));
        if (calendar != NULL && calendar->value.kind != SJS_UNDEFINED && iso_calendar(vm, calendar->value) != SJS_OK) return SJS_ERR_PROGRAM;
        date_fields f = { 0 };
        time_fields g = { 0 };
        if (read_date_fields(vm, bag, &f) != SJS_OK || read_time_fields(vm, bag, &g) != SJS_OK) return SJS_ERR_PROGRAM;
        if (overflow_rejects(vm, source.options, &f.reject) != SJS_OK) return SJS_ERR_PROGRAM;
        g.reject = f.reject;
        if (resolve_date(vm, &f, out) != SJS_OK || resolve_time(vm, &g, out) != SJS_OK) return SJS_ERR_PROGRAM;
    } else {
        return sjs_vm_fault(vm, "a PlainDateTime is made from a PlainDateTime, a PlainDate, an ISO 8601 string or an object of its fields");
    }
    return within_limits(out, true) ? SJS_OK : outside_limits(vm);
}

/* ------------------------------------------------------------------------
 * Functions: the constructors, from and compare
 */

/* The time arguments of a constructor, from `first` on: each undefined is
 * 0, else ToIntegerWithTruncation; each must be within its range. */
typedef struct {
    const sjs_leaf_call *call;
    uint32_t first;
} time_arguments;

static sjs_result read_time_arguments(sjs_vm *vm, time_arguments args, sjs_iso_date_time *d) {
    time_fields f = { .reject = true };
    for (uint32_t i = 0; i < TIME_FIELD_COUNT; i++) {
        sjs_value v = arg_of(args.call, args.first + i);
        if (v.kind == SJS_UNDEFINED) continue;
        if (integer_of(vm, v, &f.value[i]) != SJS_OK) return SJS_ERR_PROGRAM;
        f.has[i] = true;
    }
    set_time_of_day(d, 0);
    return resolve_time(vm, &f, d);
}

/* The date arguments of a constructor: year, month and day, each as
 * ToIntegerWithTruncation gives it, the date valid as it is ("reject"). */
static sjs_result read_date_arguments(sjs_vm *vm, const sjs_leaf_call *call, sjs_iso_date_time *d) {
    double fields[3];
    for (uint32_t i = 0; i < 3u; i++) {
        if (integer_of(vm, arg_of(call, i), &fields[i]) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    if (fields[1] < 1 || fields[1] > 12) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a month outside 1 to 12");
    if (fields[0] < YEAR_MIN || fields[0] > YEAR_MAX) return outside_limits(vm);
    *d = (sjs_iso_date_time){ 0 };
    d->year = (int32_t)fields[0];
    d->month = (uint8_t)fields[1];
    if (fields[2] < 1 || fields[2] > days_in_month(d->year, d->month)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a day outside its month");
    d->day = (uint8_t)fields[2];
    return SJS_OK;
}

/* A calendar argument: undefined or the ISO calendar. */
static sjs_result calendar_argument(sjs_vm *vm, sjs_value calendar) {
    return calendar.kind == SJS_UNDEFINED ? SJS_OK : iso_calendar(vm, calendar);
}

/* new Temporal.PlainDate(year, month, day, calendar),
 * new Temporal.PlainTime(hour, ..., nanosecond) and
 * new Temporal.PlainDateTime(year, month, day, hour, ..., nanosecond,
 * calendar). */
static sjs_result construct_plain(sjs_vm *vm, sjs_temporal_type type, sjs_leaf_call *call) {
    sjs_iso_date_time d = { 0 };
    if (type != SJS_TEMPORAL_PLAIN_TIME) {
        if (read_date_arguments(vm, call, &d) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    if (type == SJS_TEMPORAL_PLAIN_DATE) {
        if (calendar_argument(vm, arg_of(call, 3u)) != SJS_OK) return SJS_ERR_PROGRAM;
    } else {
        uint32_t first = type == SJS_TEMPORAL_PLAIN_TIME ? 0u : 3u;
        if (read_time_arguments(vm, (time_arguments){ call, first }, &d) != SJS_OK) return SJS_ERR_PROGRAM;
        if (type == SJS_TEMPORAL_PLAIN_DATE_TIME && calendar_argument(vm, arg_of(call, 9u)) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    if (type != SJS_TEMPORAL_PLAIN_TIME && !within_limits(&d, type == SJS_TEMPORAL_PLAIN_DATE_TIME)) return outside_limits(vm);
    return new_iso(vm, (iso_value){ type, &d }, &call->result);
}

/* A value of `type` from what from() and compare() take. */
typedef struct {
    sjs_temporal_type type;
    date_source source;
} conversion;

static sjs_result convert(sjs_vm *vm, conversion c, sjs_iso_date_time *out) {
    switch (c.type) {
    case SJS_TEMPORAL_PLAIN_DATE:
        return to_date(vm, c.source, out);
    case SJS_TEMPORAL_PLAIN_TIME:
        return to_time(vm, c.source, out);
    default:
        return to_date_time(vm, c.source, out);
    }
}

static int compare_iso(sjs_temporal_type type, const sjs_iso_date_time *a, const sjs_iso_date_time *b) {
    return type == SJS_TEMPORAL_PLAIN_DATE ? compare_dates(a, b) : type == SJS_TEMPORAL_PLAIN_TIME ? compare_times(a, b) : compare_date_times(a, b);
}

sjs_result sjs_temporal_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    static const struct {
        sjs_builtin_id id;
        sjs_temporal_type type;
    } TYPES[] = {
        { SJS_BUILTIN_TEMPORAL_PLAIN_DATE, SJS_TEMPORAL_PLAIN_DATE },
        { SJS_BUILTIN_TEMPORAL_PLAIN_TIME, SJS_TEMPORAL_PLAIN_TIME },
        { SJS_BUILTIN_TEMPORAL_PLAIN_DATE_TIME, SJS_TEMPORAL_PLAIN_DATE_TIME },
    };
    /* Each type's constructor, from and compare are three IDs in a row. */
    uint32_t t = 0;
    while (t + 1u < sizeof TYPES / sizeof TYPES[0] && id >= TYPES[t + 1u].id) t++;
    sjs_temporal_type type = TYPES[t].type;
    uint32_t which = (uint32_t)(id - TYPES[t].id);
    sjs_iso_date_time a;
    if (which == 0) return construct_plain(vm, type, call);
    if (which == 1u) {
        sjs_value item = arg_of(call, 0);
        if (convert(vm, (conversion){ type, { item, arg_of(call, 1u) } }, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        if (is_temporal(item, type)) {
            /* Equal, and immutable: the same value serves. */
            call->result = item;
            sjs_ref(item);
            return SJS_OK;
        }
        return new_iso(vm, (iso_value){ type, &a }, &call->result);
    }
    sjs_iso_date_time b;
    if (convert(vm, (conversion){ type, { arg_of(call, 0), sjs_undefined() } }, &a) != SJS_OK) return SJS_ERR_PROGRAM;
    if (convert(vm, (conversion){ type, { arg_of(call, 1u), sjs_undefined() } }, &b) != SJS_OK) return SJS_ERR_PROGRAM;
    call->result = sjs_num((double)compare_iso(type, &a, &b));
    return SJS_OK;
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

/* A plain value's getter: of its date, or of its time. */
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
    case SJS_FIELD_HOUR:
        *out = sjs_num((double)d->hour);
        return SJS_OK;
    case SJS_FIELD_MINUTE:
        *out = sjs_num((double)d->minute);
        return SJS_OK;
    case SJS_FIELD_SECOND:
        *out = sjs_num((double)d->second);
        return SJS_OK;
    case SJS_FIELD_MILLISECOND:
        *out = sjs_num((double)d->millisecond);
        return SJS_OK;
    case SJS_FIELD_MICROSECOND:
        *out = sjs_num((double)d->microsecond);
        return SJS_OK;
    case SJS_FIELD_NANOSECOND:
        *out = sjs_num((double)d->nanosecond);
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
        sjs_temporal_type type = sjs_temporal_type_of(v);
        if (type != SJS_TEMPORAL_PLAIN_DATE && type != SJS_TEMPORAL_PLAIN_TIME && type != SJS_TEMPORAL_PLAIN_DATE_TIME) {
            return sjs_vm_fault(vm, "staticjs does not build this Temporal type's getters yet");
        }
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
 * them. A bag with calendar or timeZone, or with none of the type's
 * fields, is a TypeError in JavaScript; so is a Temporal value or anything
 * not an object. */
static sjs_result iso_with(sjs_vm *vm, const sjs_temporal *t, sjs_leaf_call *call) {
    sjs_temporal_type type = (sjs_temporal_type)t->h.flags;
    const sjs_object *bag;
    if (with_bag(vm, arg_of(call, 0), &bag) != SJS_OK) return SJS_ERR_PROGRAM;
    date_fields f = { 0 };
    time_fields g = { 0 };
    if (type != SJS_TEMPORAL_PLAIN_TIME && read_date_fields(vm, bag, &f) != SJS_OK) return SJS_ERR_PROGRAM;
    if (type != SJS_TEMPORAL_PLAIN_DATE && read_time_fields(vm, bag, &g) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!f.any && !g.any) {
        return sjs_vm_fault(vm, type == SJS_TEMPORAL_PLAIN_DATE   ? "JavaScript throws a TypeError for with's fields without a year, month, monthCode or day"
                                : type == SJS_TEMPORAL_PLAIN_TIME ? "JavaScript throws a TypeError for with's fields without an hour, minute, second, millisecond, microsecond or nanosecond"
                                                                  : "JavaScript throws a TypeError for with's fields without any field of a date or a time");
    }
    if (overflow_rejects(vm, arg_of(call, 1u), &f.reject) != SJS_OK) return SJS_ERR_PROGRAM;
    g.reject = f.reject;
    sjs_iso_date_time made = t->as.iso;
    if (type != SJS_TEMPORAL_PLAIN_TIME) {
        merge_date_fields(&f, &t->as.iso);
        if (resolve_date(vm, &f, &made) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    if (type != SJS_TEMPORAL_PLAIN_DATE && resolve_time(vm, &g, &made) != SJS_OK) return SJS_ERR_PROGRAM;
    if (type == SJS_TEMPORAL_PLAIN_DATE_TIME && !within_limits(&made, true)) return outside_limits(vm);
    return new_iso(vm, (iso_value){ type, &made }, &call->result);
}

/* A date at a time: toPlainDateTime(time) of a PlainDate and
 * withPlainTime(time) of a PlainDateTime. No time is midnight; within
 * Temporal's limits. */
static sjs_result at_time(sjs_vm *vm, const sjs_temporal *t, sjs_leaf_call *call) {
    sjs_iso_date_time made = t->as.iso;
    sjs_iso_date_time time = { 0 };
    sjs_value item = arg_of(call, 0);
    if (item.kind != SJS_UNDEFINED && to_time(vm, (date_source){ item, sjs_undefined() }, &time) != SJS_OK) return SJS_ERR_PROGRAM;
    set_time_of_day(&made, time_of_day(&time));
    if (!within_limits(&made, true)) return outside_limits(vm);
    return new_iso(vm, (iso_value){ SJS_TEMPORAL_PLAIN_DATE_TIME, &made }, &call->result);
}

/* round(smallestUnit) or round({ smallestUnit, roundingIncrement,
 * roundingMode }): a PlainTime to an hour at most, a PlainDateTime to a
 * day. smallestUnit is required (a TypeError without options, a
 * RangeError without the unit); roundingMode is halfExpand by default. */
static sjs_result iso_round(sjs_vm *vm, const sjs_temporal *t, sjs_leaf_call *call) {
    sjs_temporal_type type = (sjs_temporal_type)t->h.flags;
    unit_range range = { UNIT_NANOSECOND, type == SJS_TEMPORAL_PLAIN_TIME ? UNIT_HOUR : UNIT_DAY };
    sjs_value options = arg_of(call, 0);
    rounding r = { 1, MODE_HALF_EXPAND };
    unit_choice unit = { UNIT_NANOSECOND, false };
    if (options.kind == SJS_STRING) {
        unit.given = true;
        if (unit_in_range(vm, (unit_text){ sjs_string_view(SJS_AS_STRING(options)), range }, &unit.unit) != SJS_OK) return SJS_ERR_PROGRAM;
    } else {
        if (options.kind == SJS_UNDEFINED) return sjs_vm_fault(vm, "JavaScript throws a TypeError for round without a smallestUnit");
        if (increment_option(vm, options, &r.increment) != SJS_OK || mode_option(vm, options, &r.mode) != SJS_OK) return SJS_ERR_PROGRAM;
        if (smallest_unit(vm, (unit_option){ options, range }, &unit) != SJS_OK) return SJS_ERR_PROGRAM;
        if (!unit.given) return sjs_vm_fault(vm, "JavaScript throws a RangeError for round's options without a smallestUnit");
    }
    if (check_increment(vm, r.increment, unit.unit) != SJS_OK) return SJS_ERR_PROGRAM;
    r.increment *= UNIT_NANOSECONDS[unit.unit];
    sjs_iso_date_time made = t->as.iso;
    if (!round_iso(&made, r, type == SJS_TEMPORAL_PLAIN_DATE_TIME)) return outside_limits(vm);
    return new_iso(vm, (iso_value){ type, &made }, &call->result);
}

/* toString(options). A date's and a date and time's calendarName: "auto"
 * and "never" write no calendar, "always" writes [u-ca=iso8601] and
 * "critical" [!u-ca=iso8601]. A time's and a date and time's
 * fractionalSecondDigits ("auto", or a number floored to 0 to 9),
 * smallestUnit (minute to nanosecond, which overrides the digits) and
 * roundingMode (trunc by default): the time rounds to the digits written. */
typedef struct {
    const sjs_temporal *value;
    sjs_value options;
} text_request;

typedef struct {
    int32_t digits;   /* DIGITS_MINUTE, DIGITS_AUTO, or 0 to 9 */
    rounding r;
} text_precision;

static sjs_result digits_option(sjs_vm *vm, sjs_value options, int32_t *digits) {
    sjs_value v;
    *digits = DIGITS_AUTO;
    if (option_of(vm, (option_name){ options, SJS_STR("fractionalSecondDigits") }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    if (v.kind == SJS_UNDEFINED) return SJS_OK;
    if (v.kind == SJS_NUMBER) {
        double d = sjs_number_floor(v.as.number);
        if (sjs_number_is_finite(d) && d >= 0 && d <= 9) {
            *digits = (int32_t)d;
            return SJS_OK;
        }
    } else if (v.kind == SJS_STRING && sjs_str_equal(sjs_string_view(SJS_AS_STRING(v)), SJS_STR("auto"))) {
        return SJS_OK;
    }
    return sjs_vm_fault(vm, "JavaScript throws a RangeError for a fractionalSecondDigits other than \"auto\" or a number from 0 to 9");
}

static sjs_result precision_of(sjs_vm *vm, sjs_value options, text_precision *out) {
    *out = (text_precision){ DIGITS_AUTO, { 1, MODE_TRUNC } };
    if (digits_option(vm, options, &out->digits) != SJS_OK || mode_option(vm, options, &out->r.mode) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_value v;
    if (option_of(vm, (option_name){ options, SJS_STR("smallestUnit") }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    if (v.kind == SJS_STRING && sjs_str_equal(sjs_string_view(SJS_AS_STRING(v)), SJS_STR("auto"))) {
        /* temporal-polyfill reads it as no unit, @js-temporal/polyfill
         * refuses it. */
        return sjs_vm_fault(vm, "staticjs refuses toString's smallestUnit \"auto\", which the polyfills read differently");
    }
    unit_choice unit;
    if (smallest_unit(vm, (unit_option){ options, { UNIT_NANOSECOND, UNIT_MINUTE } }, &unit) != SJS_OK) return SJS_ERR_PROGRAM;
    if (unit.given) {
        static const int32_t DIGITS[] = { 9, 6, 3, 0, DIGITS_MINUTE };
        out->digits = DIGITS[unit.unit];
    }
    static const int64_t SCALE[] = { 1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1 };
    out->r.increment = out->digits == DIGITS_MINUTE ? UNIT_NANOSECONDS[UNIT_MINUTE] : out->digits == DIGITS_AUTO ? 1 : SCALE[out->digits];
    return SJS_OK;
}

static sjs_result iso_to_string(sjs_vm *vm, text_request request, sjs_value *out) {
    static const char *const CHOICES[] = { "auto", "never", "always", "critical" };
    const sjs_temporal *t = request.value;
    sjs_temporal_type type = (sjs_temporal_type)t->h.flags;
    uint32_t choice = 0;
    if (type != SJS_TEMPORAL_PLAIN_TIME) {
        if (string_option(vm, (option_spec){ request.options, SJS_STR("calendarName"), CHOICES, 4u }, &choice) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    text_precision precision = { DIGITS_AUTO, { 1, MODE_TRUNC } };
    sjs_iso_date_time d = t->as.iso;
    if (type != SJS_TEMPORAL_PLAIN_DATE) {
        if (precision_of(vm, request.options, &precision) != SJS_OK) return SJS_ERR_PROGRAM;
        if (!round_iso(&d, precision.r, type == SJS_TEMPORAL_PLAIN_DATE_TIME)) return outside_limits(vm);
    }
    char text[SJS_TEMPORAL_TEXT_MAX + 16u];
    uint32_t n = put_iso(text, (iso_value){ type, &d }, precision.digits);
    sjs_str suffix = choice == 2u ? SJS_STR("[u-ca=iso8601]") : choice == 3u ? SJS_STR("[!u-ca=iso8601]") : SJS_STR("");
    sjs_copy(text + n, suffix.ptr, suffix.length);
    vm->fuel_used += n + suffix.length;
    return temporal_string(vm, (sjs_str){ text, n + suffix.length }, out);
}

sjs_result sjs_temporal_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    if (call->receiver.kind != SJS_TEMPORAL) return sjs_vm_fault(vm, "this method is a Temporal value's");
    const sjs_temporal *t = SJS_AS_TEMPORAL(call->receiver);
    sjs_temporal_type type = (sjs_temporal_type)t->h.flags;
    if (type != SJS_TEMPORAL_PLAIN_DATE && type != SJS_TEMPORAL_PLAIN_TIME && type != SJS_TEMPORAL_PLAIN_DATE_TIME) {
        return sjs_vm_fault(vm, "staticjs does not build this Temporal type's methods yet");
    }
    switch (id) {
    case SJS_METHOD_TEMPORAL_EQUALS: {
        sjs_iso_date_time other;
        if (convert(vm, (conversion){ type, { arg_of(call, 0), sjs_undefined() } }, &other) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_boolean(compare_iso(type, &t->as.iso, &other) == 0);
        return SJS_OK;
    }
    case SJS_METHOD_TEMPORAL_WITH:
        return iso_with(vm, t, call);
    case SJS_METHOD_TEMPORAL_ROUND:
        return iso_round(vm, t, call);
    case SJS_METHOD_TEMPORAL_TO_STRING:
        return iso_to_string(vm, (text_request){ t, arg_of(call, 0) }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_JSON:
        return iso_to_string(vm, (text_request){ t, sjs_undefined() }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_PLAIN_DATE_TIME:
    case SJS_METHOD_TEMPORAL_WITH_PLAIN_TIME:
        return at_time(vm, t, call);
    case SJS_METHOD_TEMPORAL_TO_PLAIN_DATE:
        return new_iso(vm, (iso_value){ SJS_TEMPORAL_PLAIN_DATE, &t->as.iso }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_PLAIN_TIME:
        return new_iso(vm, (iso_value){ SJS_TEMPORAL_PLAIN_TIME, &t->as.iso }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_LOCALE_STRING:
        return sjs_vm_fault(vm, "toLocaleString depends on locale data, which staticjs does not have");
    case SJS_METHOD_TEMPORAL_VALUE_OF:
        return sjs_vm_fault(vm, "JavaScript throws a TypeError for valueOf of a Temporal value: compare with equals or compare");
    case SJS_METHOD_TEMPORAL_ADD:
    case SJS_METHOD_TEMPORAL_SUBTRACT:
    case SJS_METHOD_TEMPORAL_UNTIL:
    case SJS_METHOD_TEMPORAL_SINCE:
        return sjs_vm_fault(vm, "staticjs does not build add, subtract, until and since of a PlainDate, a PlainTime or a PlainDateTime yet: they take or give a Temporal.Duration");
    case SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME:
        return sjs_vm_fault(vm, "staticjs has no Temporal.ZonedDateTime: it needs time zone data");
    case SJS_METHOD_TEMPORAL_WITH_CALENDAR:
        return sjs_vm_fault(vm, "staticjs has only the ISO 8601 calendar, and no withCalendar");
    default:
        return sjs_vm_fault(vm, "staticjs does not have Temporal.PlainYearMonth or Temporal.PlainMonthDay");
    }
}
