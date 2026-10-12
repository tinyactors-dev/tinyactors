/*
 * Temporal values (docs/temporal.md): Temporal.PlainDate, PlainTime and
 * PlainDateTime in the ISO 8601 calendar, Temporal.Duration and
 * Temporal.Instant (UTC only), their constructors, from, compare, getters
 * and methods. Temporal.Now builds on the same kind, SJS_TEMPORAL, and the
 * same tables.
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
    /* RoundTime counts the quantity from the start of the unit above the
     * one rounded to (a minute's from the hour, an hour's from midnight).
     * An increment is always less than that unit and divides it, so the
     * start is a multiple of the increment: only halfEven's parity can
     * tell it from counting from midnight. */
    static const int64_t ABOVE[] = { 1000, 1000000, 1000000000, INT64_C(60000000000), INT64_C(3600000000000) };
    int64_t above = NS_PER_DAY;
    for (uint32_t i = 0; i < sizeof ABOVE / sizeof ABOVE[0]; i++) {
        if (r.increment < ABOVE[i]) {
            above = ABOVE[i];
            break;
        }
    }
    int64_t of_day = time_of_day(d);
    int64_t base = of_day - of_day % above;
    int64_t ns = base + round_to(of_day - base, r);
    if (ns == NS_PER_DAY) {
        ns = 0;
        if (dated) date_of_days(epoch_days(d->year, d->month, d->day) + 1, d);
    }
    set_time_of_day(d, ns);
    return !dated || within_limits(d, true);
}

/* ------------------------------------------------------------------------
 * Wide integers: nanoseconds past an int64. A Duration's time reaches 2^53
 * seconds (9 x 10^24 ns), an epoch 8.64 x 10^21 ns. A sign and a 128-bit
 * magnitude of two 64-bit halves, every operation built from 64-bit ones:
 * no 128-bit type, whose multiplication and division need a library
 * (check-freestanding refuses them).
 */

typedef struct {
    uint64_t hi;
    uint64_t lo;
} u128;

static u128 u128_add(u128 a, u128 b) {
    u128 r = { a.hi + b.hi, a.lo + b.lo };
    if (r.lo < a.lo) r.hi++;
    return r;
}

/* a - b, b no greater than a. */
static u128 u128_sub(u128 a, u128 b) {
    u128 r = { a.hi - b.hi, a.lo - b.lo };
    if (a.lo < b.lo) r.hi--;
    return r;
}

static int u128_cmp(u128 a, u128 b) {
    if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}

static bool u128_zero(u128 a) {
    return (a.hi | a.lo) == 0;
}

/* The full product of two 64-bit numbers, from 32-bit halves. */
static u128 mul_64(uint64_t a, uint64_t b) {
    const uint64_t mask = 0xffffffffu;
    uint64_t a0 = a & mask, a1 = a >> 32, b0 = b & mask, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t middle = (p00 >> 32) + (p01 & mask) + (p10 & mask);
    return (u128){ p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32), (middle << 32) | (p00 & mask) };
}

/* a * b, which must fit. */
static u128 u128_mul(u128 a, uint64_t b) {
    u128 r = mul_64(a.lo, b);
    r.hi += a.hi * b;
    return r;
}

/* The number of bits `a` needs: 0 for 0. */
static uint32_t u128_bits(u128 a) {
    uint32_t n = 0;
    uint64_t top = a.hi != 0 ? a.hi : a.lo;
    while (top != 0) {
        top >>= 1;
        n++;
    }
    return a.hi != 0 ? n + 64u : n;
}

static uint64_t u128_bit(u128 a, uint32_t i) {
    return i >= 64u ? (a.hi >> (i - 64u)) & 1u : (a.lo >> i) & 1u;
}

static u128 u128_shl1(u128 a, uint64_t bit) {
    return (u128){ (a.hi << 1) | (a.lo >> 63), (a.lo << 1) | bit };
}

/* a / b, and a % b into `rest`; b is not zero. Bit by bit beyond 64 bits:
 * a quotient of an epoch or a Duration has at most 84. */
static u128 u128_divmod(u128 a, u128 b, u128 *rest) {
    if (a.hi == 0 && b.hi == 0) {
        *rest = (u128){ 0, a.lo % b.lo };
        return (u128){ 0, a.lo / b.lo };
    }
    u128 q = { 0, 0 };
    u128 r = { 0, 0 };
    for (uint32_t i = u128_bits(a); i-- > 0;) {
        r = u128_shl1(r, u128_bit(a, i));
        q = u128_shl1(q, 0);
        if (u128_cmp(r, b) >= 0) {
            r = u128_sub(r, b);
            q.lo |= 1u;
        }
    }
    *rest = r;
    return q;
}

/* A signed wide integer: its magnitude and sign (never a negative zero). */
typedef struct {
    u128 m;
    bool negative;
} wide;

static wide wide_make(u128 m, bool negative) {
    return (wide){ m, negative && !u128_zero(m) };
}

static wide wide_of(int64_t v) {
    return v < 0 ? (wide){ { 0, (uint64_t)0 - (uint64_t)v }, true } : (wide){ { 0, (uint64_t)v }, false };
}

static int wide_sign(wide a) {
    return u128_zero(a.m) ? 0 : a.negative ? -1 : 1;
}

static wide wide_neg(wide a) {
    return wide_make(a.m, !a.negative);
}

static wide wide_add(wide a, wide b) {
    if (a.negative == b.negative) return wide_make(u128_add(a.m, b.m), a.negative);
    if (u128_cmp(a.m, b.m) >= 0) return wide_make(u128_sub(a.m, b.m), a.negative);
    return wide_make(u128_sub(b.m, a.m), b.negative);
}

static wide wide_sub(wide a, wide b) {
    return wide_add(a, wide_neg(b));
}

static int wide_cmp(wide a, wide b) {
    return wide_sign(wide_sub(a, b));
}

/* a * b, which must fit. */
static wide wide_mul(wide a, int64_t b) {
    uint64_t magnitude = b < 0 ? (uint64_t)0 - (uint64_t)b : (uint64_t)b;
    return wide_make(u128_mul(a.m, magnitude), a.negative != (b < 0));
}

/* a / d truncated, and the rest (of a's sign); d is not zero. */
static wide wide_quotient(wide a, u128 d, wide *rest) {
    u128 r;
    u128 q = u128_divmod(a.m, d, &r);
    *rest = wide_make(r, a.negative);
    return wide_make(q, a.negative);
}

/* a / d rounded toward negative infinity, and the rest, from 0 to d. */
static wide wide_floor(wide a, u128 d, wide *rest) {
    wide q = wide_quotient(a, d, rest);
    if (wide_sign(*rest) < 0) {
        q = wide_sub(q, wide_of(1));
        *rest = wide_add(*rest, wide_make(d, false));
    }
    return q;
}

/* The value of a wide integer that fits an int64. */
static int64_t wide_int64(wide a) {
    return a.negative ? (int64_t)((uint64_t)0 - a.m.lo) : (int64_t)a.m.lo;
}

/* p / q as the nearest double, ties to even, q not zero: the integer part,
 * then as many bits of the fraction as make 64, and whether more follow. */
static double ratio_double(wide p, wide q) {
    if (u128_zero(p.m)) return 0;
    u128 r;
    u128 n = u128_divmod(p.m, q.m, &r);
    int32_t e2 = 0;
    uint32_t bits = u128_bits(n);
    bool sticky = !u128_zero(r);
    uint64_t top;
    if (bits > 64u) {
        uint32_t shift = bits - 64u;
        top = shift == 64u ? n.hi : (n.hi << (64u - shift)) | (n.lo >> shift);
        sticky = sticky || (shift == 64u ? n.lo : n.lo << (64u - shift)) != 0;
        e2 = (int32_t)shift;
    } else {
        top = n.lo;
        while ((top >> 63) == 0) {
            r = u128_shl1(r, 0);
            uint64_t bit = u128_cmp(r, q.m) >= 0 ? 1u : 0u;
            if (bit != 0) r = u128_sub(r, q.m);
            top = (top << 1) | bit;
            e2--;
        }
        sticky = !u128_zero(r);
    }
    double d = sjs_number_from_top((sjs_number_top){ top, e2, sticky });
    return p.negative != q.negative ? -d : d;
}

static double wide_double(wide a) {
    return ratio_double(a, wide_of(1));
}

typedef union {
    double d;
    uint64_t u;
} double_word;

/* An integral double below 2^100 in magnitude, exactly. */
static wide wide_of_double(double x) {
    double_word w;
    w.d = x;
    int32_t exponent = (int32_t)((w.u >> 52) & 0x7ffu);
    if (exponent == 0) return wide_of(0);
    uint64_t mantissa = (w.u & ((UINT64_C(1) << 52) - 1u)) | (UINT64_C(1) << 52);
    int32_t shift = exponent - 1075;
    u128 m = { 0, mantissa };
    if (shift < 0) {
        m.lo = shift <= -64 ? 0 : mantissa >> (uint32_t)-shift;
    } else if (shift > 0) {
        m.hi = shift >= 64 ? mantissa << (uint32_t)(shift - 64) : mantissa >> (uint32_t)(64 - shift);
        m.lo = shift >= 64 ? 0 : mantissa << (uint32_t)shift;
    }
    return wide_make(m, (w.u >> 63) != 0);
}

/* a * b, which must fit. */
static u128 u128_product(u128 a, u128 b) {
    return b.hi == 0 ? u128_mul(a, b.lo) : u128_mul(b, a.lo);
}

/* Does rounding by `mode` take the multiple further from zero? `cmp`
 * compares the rest with half the increment; `even`, whether the nearer
 * multiple is an even one (GetUnsignedRoundingMode, then
 * ApplyUnsignedRoundingMode). */
typedef struct {
    rounding_mode mode;
    bool negative;
    int cmp;
    bool even;
} rounding_case;

static bool rounds_away(rounding_case c) {
    switch (c.mode) {
    case MODE_CEIL:
        return !c.negative;
    case MODE_FLOOR:
        return c.negative;
    case MODE_EXPAND:
        return true;
    case MODE_TRUNC:
        return false;
    default:
        break;
    }
    if (c.cmp != 0) return c.cmp > 0;
    switch (c.mode) {
    case MODE_HALF_CEIL:
        return !c.negative;
    case MODE_HALF_FLOOR:
        return c.negative;
    case MODE_HALF_EXPAND:
        return true;
    case MODE_HALF_TRUNC:
        return false;
    default:
        return !c.even;
    }
}

/* x rounded to a multiple of `increment` by `mode`, relative to its sign. */
typedef struct {
    u128 increment;
    rounding_mode mode;
} wide_rounding;

static wide round_wide(wide x, wide_rounding r) {
    u128 rest;
    u128 q = u128_divmod(x.m, r.increment, &rest);
    if (u128_zero(rest)) return x;
    int cmp = u128_cmp(u128_add(rest, rest), r.increment);
    if (rounds_away((rounding_case){ r.mode, x.negative, cmp, (q.lo & 1u) == 0 })) q = u128_add(q, (u128){ 0, 1 });
    return wide_make(u128_product(q, r.increment), x.negative);
}

/* RoundNumberToIncrement of an integer, as round_wide. */
static int64_t round_number(int64_t x, int64_t increment, rounding_mode mode) {
    return wide_int64(round_wide(wide_of(x), (wide_rounding){ { 0, (uint64_t)increment }, mode }));
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

static uint32_t put_duration(char *out, const double *f, int32_t digits);
static uint32_t put_instant_text(const sjs_temporal *t, char *out);

uint32_t sjs_temporal_text(const sjs_temporal *t, char *out) {
    switch ((sjs_temporal_type)t->h.flags) {
    case SJS_TEMPORAL_PLAIN_DATE:
    case SJS_TEMPORAL_PLAIN_TIME:
    case SJS_TEMPORAL_PLAIN_DATE_TIME:
        return put_iso(out, (iso_value){ (sjs_temporal_type)t->h.flags, &t->as.iso }, DIGITS_AUTO);
    case SJS_TEMPORAL_DURATION:
        return put_duration(out, t->as.duration, DIGITS_AUTO);
    case SJS_TEMPORAL_DATE:
        return sjs_date_iso_text(t->as.time, out);
    default:
        return put_instant_text(t, out);
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
    case SJS_TEMPORAL_DATE:
        return SJS_STR("Date");
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
    bool time_zone;      /* scan_annotations read a time zone annotation */
    bool any_calendar;   /* an Instant's: a calendar annotation is ignored */
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
        iso_scan s = { id, 1u, false, false };
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
            s->time_zone = true;
            continue;
        }
        sjs_str key = { s->text.ptr + start, equals - start };
        sjs_str value = { s->text.ptr + equals + 1u, s->at - 1u - (equals + 1u) };
        if (!annotation_key(key) || !annotation_value(value)) return false;
        if (sjs_str_equal(key, SJS_STR("u-ca"))) {
            if (++calendars == 1u && !s->any_calendar && !equal_ignoring_case(value, SJS_STR("iso8601"))) return false;
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
    bool zoned;   /* with a time zone annotation: a ZonedDateTime's */
} iso_parse;

/* A date, YYYY-MM-DD or YYYYMMDD or with a sign and six digits, into `d`. */
static bool scan_date(iso_scan *s, sjs_iso_date_time *d) {
    uint32_t year;
    int sign = scan_peek(s);
    if (sign == '+' || sign == '-') {
        s->at++;
        if (!scan_digits(s, 6u, &year) || (sign == '-' && year == 0)) return false;
    } else if (!scan_digits(s, 4u, &year)) {
        return false;
    }
    d->year = sign == '-' ? -(int32_t)year : (int32_t)year;
    bool extended = scan_take(s, '-');
    uint32_t month;
    uint32_t day;
    if (!scan_digits(s, 2u, &month) || (extended && !scan_take(s, '-')) || !scan_digits(s, 2u, &day)) return false;
    if (month < 1u || month > 12u || day < 1u || day > days_in_month(d->year, month)) return false;
    d->month = (uint8_t)month;
    d->day = (uint8_t)day;
    return true;
}

static bool parse_iso(sjs_str text, iso_parse *out) {
    iso_scan s = { text, 0, false, false };
    sjs_iso_date_time *d = &out->when;
    *out = (iso_parse){ 0 };
    if (!scan_date(&s, d)) return false;
    int separator = scan_peek(&s);
    if (separator == 'T' || separator == 't' || separator == ' ') {
        s.at++;
        clock_parts time;
        if (!scan_clock(&s, (clock_grammar){ true, 60u }, &time)) return false;
        out->has_time = true;
        set_clock(d, &time);
        if (!scan_offset(&s)) return false;
    }
    if (!scan_annotations(&s)) return false;
    out->zoned = s.time_zone;
    return true;
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
    iso_scan s = { text, 0, false, false };
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

/* ------------------------------------------------------------------------
 * Durations: ten fields, years to nanoseconds, each an integer in a
 * double, all of one sign. Arithmetic goes through an internal form
 * (ToInternalDurationRecord): the date units as int64s and the time units
 * as one wide integer of nanoseconds.
 */

#define DURATION_FIELD_COUNT 10u
static const char *const DURATION_FIELDS[DURATION_FIELD_COUNT] = {
    "years", "months", "weeks", "days", "hours", "minutes", "seconds", "milliseconds", "microseconds", "nanoseconds",
};

/* Field i holds the unit UNIT_YEAR - i. */
#define FIELD_OF_UNIT(unit) (UNIT_YEAR - (unit))

static sjs_result outside_duration(sjs_vm *vm) {
    return sjs_vm_fault(vm, "JavaScript throws a RangeError for a Duration whose days and time reach 2^53 seconds");
}

static double field_magnitude(double x) {
    return x < 0 ? -x : x;
}

/* A Duration's sign: of its first field that is not zero. */
static int duration_sign(const double f[DURATION_FIELD_COUNT]) {
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) {
        if (f[i] != 0) return f[i] < 0 ? -1 : 1;
    }
    return 0;
}

/* The largest unit whose field is not zero (DefaultTemporalLargestUnit). */
static time_unit largest_unit_of(const double f[DURATION_FIELD_COUNT]) {
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT - 1u; i++) {
        if (f[i] != 0) return (time_unit)(UNIT_YEAR - i);
    }
    return UNIT_NANOSECOND;
}

static bool is_calendar_unit(time_unit unit) {
    return unit >= UNIT_WEEK;
}

/* The time fields' nanoseconds, the days' as 24 hours with `days`. Only of
 * fields check_duration accepts (or as small): every product is below 10^25. */
static wide duration_time(const double f[DURATION_FIELD_COUNT], bool days) {
    wide total = wide_of(0);
    for (uint32_t i = days ? 3u : 4u; i < DURATION_FIELD_COUNT; i++) {
        total = wide_add(total, wide_mul(wide_of_double(f[i]), UNIT_NANOSECONDS[UNIT_YEAR - i]));
    }
    return total;
}

/* Below 2^53 seconds, as 9,007,199,254,740,991.999999999 at most. */
static bool time_within_limits(wide ns) {
    wide rest;
    wide seconds = wide_quotient(ns, (u128){ 0, 1000000000u }, &rest);
    return seconds.m.hi == 0 && seconds.m.lo < (UINT64_C(1) << 53);
}

/* IsValidDuration: finite integers of one sign, years, months and weeks
 * below 2^32, and the days and time below 2^53 seconds, exactly (as
 * temporal-polyfill computes them, in BigInts). */
static sjs_result check_duration(sjs_vm *vm, const double f[DURATION_FIELD_COUNT]) {
    int sign = 0;
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) {
        if (!sjs_number_is_finite(f[i])) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a Duration field that is not finite");
        if (f[i] == 0) continue;
        int s = f[i] < 0 ? -1 : 1;
        if (sign != 0 && s != sign) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a Duration whose fields have different signs");
        sign = s;
    }
    for (uint32_t i = 0; i < 3u; i++) {
        if (field_magnitude(f[i]) >= 4294967296.0) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a Duration's years, months or weeks of 2^32 or more");
    }
    for (uint32_t i = 3u; i < DURATION_FIELD_COUNT; i++) {
        /* Far past the limit, which is below 10^25 ns: not added. */
        if (field_magnitude(f[i]) * (double)UNIT_NANOSECONDS[UNIT_YEAR - i] >= 1e25) return outside_duration(vm);
    }
    return time_within_limits(duration_time(f, true)) ? SJS_OK : outside_duration(vm);
}

/* A Duration of `f`'s fields, checked; a zero field is +0. */
static sjs_result new_duration(sjs_vm *vm, const double f[DURATION_FIELD_COUNT], sjs_value *out) {
    if (check_duration(vm, f) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_temporal *t;
    sjs_result r = sjs_temporal_make(vm->context, SJS_TEMPORAL_DURATION, &t);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) t->as.duration[i] = f[i] + 0.0;
    *out = sjs_heap_value(SJS_TEMPORAL, &t->h);
    return SJS_OK;
}

static void negate_fields(double f[DURATION_FIELD_COUNT]) {
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) f[i] = 0.0 - f[i];
}

/* The internal form: a date part and a time part. */
typedef struct {
    int64_t years;
    int64_t months;
    int64_t weeks;
    int64_t days;
} date_duration;

typedef struct {
    date_duration date;
    wide time;
} internal_duration;

/* ToInternalDurationRecord, and with `days` ...With24HourDays: the days
 * in the time as 24 hours each. */
static internal_duration internal_of(const double f[DURATION_FIELD_COUNT], bool days) {
    internal_duration d = { { (int64_t)f[0], (int64_t)f[1], (int64_t)f[2], days ? 0 : (int64_t)f[3] }, duration_time(f, days) };
    return d;
}

static int date_duration_sign(const date_duration *d) {
    int64_t fields[4] = { d->years, d->months, d->weeks, d->days };
    for (uint32_t i = 0; i < 4u; i++) {
        if (fields[i] != 0) return fields[i] < 0 ? -1 : 1;
    }
    return 0;
}

static int internal_sign(const internal_duration *d) {
    int sign = date_duration_sign(&d->date);
    return sign != 0 ? sign : wide_sign(d->time);
}

/* TemporalDurationFromInternal: the time balanced up to `largest` (a day
 * at most, 24 hours), its largest unit's field rounded to a double when it
 * is past 2^53; the fields unchecked. */
typedef struct {
    internal_duration duration;
    time_unit largest;
} duration_balance;

static void fields_of_internal(duration_balance b, double f[DURATION_FIELD_COUNT]) {
    const internal_duration *d = &b.duration;
    double date[4] = { (double)d->date.years, (double)d->date.months, (double)d->date.weeks, (double)d->date.days };
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) f[i] = i < 4u ? date[i] : 0;
    int sign = wide_sign(d->time);
    wide rest = wide_make(d->time.m, false);
    for (uint32_t u = b.largest > UNIT_DAY ? UNIT_DAY : b.largest;; u--) {
        wide q = wide_quotient(rest, (u128){ 0, (uint64_t)UNIT_NANOSECONDS[u] }, &rest);
        double v = wide_double(q);
        f[FIELD_OF_UNIT(u)] += sign < 0 ? -v : v;
        if (u == UNIT_NANOSECOND) break;
    }
}

/* The Duration of an internal one, checked. */
static sjs_result duration_of_internal(sjs_vm *vm, duration_balance b, sjs_value *out) {
    double f[DURATION_FIELD_COUNT];
    fields_of_internal(b, f);
    return new_duration(vm, f, out);
}


/* A Duration's text (TemporalDurationToString): the sign, P, each date
 * field that is not zero, then T and the hours and minutes that are not
 * zero and the seconds, with the smaller fields as their fraction, when
 * they are not zero, or no larger field is, or `digits` asks for a
 * fraction: DIGITS_AUTO or 0 to 9. At most about 110 bytes. */
static uint32_t put_duration(char *out, const double *f, int32_t digits) {
    static const char LETTERS[] = "YMWDHM";
    double seconds_fields[DURATION_FIELD_COUNT] = { 0, 0, 0, 0, 0, 0, f[6], f[7], f[8], f[9] };
    wide seconds = duration_time(seconds_fields, false);
    bool with_seconds = !u128_zero(seconds.m) || largest_unit_of(f) <= UNIT_SECOND || digits != DIGITS_AUTO;
    uint32_t n = 0;
    if (duration_sign(f) < 0) out[n++] = '-';
    out[n++] = 'P';
    for (uint32_t i = 0; i < 6u; i++) {
        if (i == 4u && (f[4] != 0 || f[5] != 0 || with_seconds)) out[n++] = 'T';
        if (f[i] == 0) continue;
        n += put_digits(out + n, (uint64_t)field_magnitude(f[i]), 1u);
        out[n++] = LETTERS[i];
    }
    if (!with_seconds) return n;
    u128 fraction;
    u128 whole = u128_divmod(seconds.m, (u128){ 0, 1000000000u }, &fraction);
    n += put_digits(out + n, whole.lo, 1u);
    char room[9];
    put_digits(room, fraction.lo, 9u);
    uint32_t count = (uint32_t)digits;
    if (digits == DIGITS_AUTO) {
        count = 9u;
        while (count > 0 && room[count - 1u] == '0') count--;
    }
    if (count > 0) {
        out[n++] = '.';
        for (uint32_t i = 0; i < count; i++) out[n++] = room[i];
    }
    out[n++] = 'S';
    return n;
}

/* An ISO 8601 duration, as both polyfills read one: a sign (+ or -), P,
 * then whole years, months, weeks and days, each digits and its letter;
 * then T and hours, minutes and seconds, the last of them with a fraction
 * of 1 to 9 digits after . or , if any. The letters in either case; at
 * least one part, and something after a T. A fraction is nanoseconds of
 * its unit, spread over the smaller fields. */
typedef struct {
    uint64_t whole;
    uint32_t fraction;   /* nanoseconds of a unit, 0 to 999,999,999 */
    bool fractional;
    char letter;         /* uppercase */
} duration_part;

/* Digits, then a fraction where `fractions` allows, then a letter. The
 * value of digits past 2^64 is 2^64 - 1: a Duration's limits are far
 * below it, so it faults as theirs does. */
static bool scan_duration_part(iso_scan *s, bool fractions, duration_part *out) {
    *out = (duration_part){ 0 };
    if (!ascii_digit(scan_peek(s))) return false;
    while (ascii_digit(scan_peek(s))) {
        uint64_t digit = (uint64_t)(scan_peek(s) - '0');
        out->whole = out->whole > (UINT64_MAX - digit) / 10u ? UINT64_MAX : out->whole * 10u + digit;
        s->at++;
    }
    if (fractions && (scan_peek(s) == '.' || scan_peek(s) == ',')) {
        if (!scan_fraction(s, &out->fraction)) return false;
        out->fractional = true;
    }
    int c = scan_peek(s);
    if (!ascii_letter(c)) return false;
    out->letter = (char)(c >= 'a' ? c - 'a' + 'A' : c);
    s->at++;
    return true;
}

static bool parse_duration(sjs_str text, double f[DURATION_FIELD_COUNT]) {
    static const char DATE_LETTERS[] = "YMWD";
    static const char TIME_LETTERS[] = "HMS";
    iso_scan s = { text, 0, false, false };
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) f[i] = 0;
    double sign = 1;
    if (scan_take(&s, '-')) {
        sign = -1;
    } else {
        scan_take(&s, '+');
    }
    if (!scan_take(&s, 'P') && !scan_take(&s, 'p')) return false;
    bool any = false;
    uint32_t next = 0;
    duration_part part;
    while (s.at < text.length && scan_peek(&s) != 'T' && scan_peek(&s) != 't') {
        if (!scan_duration_part(&s, false, &part)) return false;
        while (next < 4u && DATE_LETTERS[next] != part.letter) next++;
        if (next == 4u) return false;
        f[next++] = (double)part.whole;
        any = true;
    }
    uint64_t excess = 0;   /* a fraction's nanoseconds */
    if (s.at < text.length) {
        s.at++;
        if (s.at == text.length) return false;
        next = 0;
        bool fraction = false;
        while (s.at < text.length) {
            if (fraction || !scan_duration_part(&s, true, &part)) return false;
            while (next < 3u && TIME_LETTERS[next] != part.letter) next++;
            if (next == 3u) return false;
            f[4u + next] = (double)part.whole;
            static const uint64_t UNIT[] = { 3600u, 60u, 1u };
            excess = (uint64_t)part.fraction * UNIT[next];
            fraction = part.fractional;
            next++;
            any = true;
        }
    }
    if (!any) return false;
    f[9] = (double)(excess % 1000u);
    f[8] = (double)(excess / 1000u % 1000u);
    f[7] = (double)(excess / 1000000u % 1000u);
    f[6] += (double)(excess / 1000000000u % 60u);
    f[5] += (double)(excess / 60000000000u);
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) f[i] = f[i] * sign + 0.0;
    return true;
}

/* ToIntegerIfIntegral: a number, a boolean or null that is an integer; a
 * fraction, NaN or an infinity throws a RangeError in JavaScript; a
 * string, an array or an object it would convert. */
static sjs_result integral_of(sjs_vm *vm, sjs_value v, double *out) {
    double d;
    if (sjs_vm_to_number(vm, v, &d) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!sjs_number_is_integer(d)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a Duration field that is not an integer");
    *out = d + 0.0;
    return SJS_OK;
}

/* ToTemporalPartialDurationRecord: the fields an object has (not
 * undefined), at least one; `f` keeps the others. A Duration's are all
 * its fields. */
static sjs_result partial_duration(sjs_vm *vm, sjs_value item, double f[DURATION_FIELD_COUNT]) {
    if (is_temporal(item, SJS_TEMPORAL_DURATION)) {
        for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) f[i] = SJS_AS_TEMPORAL(item)->as.duration[i];
        return SJS_OK;
    }
    if (item.kind != SJS_OBJECT) return sjs_vm_fault(vm, "a Duration's fields are an object of years, months, ..., nanoseconds");
    bool any = false;
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) {
        const sjs_entry *e = sjs_object_find(SJS_AS_OBJECT(item), sjs_cstr(DURATION_FIELDS[i]));
        if (e == NULL || e->value.kind == SJS_UNDEFINED) continue;
        if (integral_of(vm, e->value, &f[i]) != SJS_OK) return SJS_ERR_PROGRAM;
        any = true;
    }
    if (!any) return sjs_vm_fault(vm, "JavaScript throws a TypeError for a Duration's fields without years, months, ..., or nanoseconds");
    return SJS_OK;
}

/* ToTemporalDuration: a Duration's fields, a string's, or an object's,
 * the fields it lacks 0; checked. */
static sjs_result to_duration(sjs_vm *vm, sjs_value item, double f[DURATION_FIELD_COUNT]) {
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) f[i] = 0;
    if (item.kind == SJS_STRING) {
        sjs_str text = parsed_text(vm, item);
        if (!parse_duration(text, f)) return unparsed(vm, "staticjs reads a Duration from an ISO 8601 duration, and this is not one", text);
    } else if (partial_duration(vm, item, f) != SJS_OK) {
        return SJS_ERR_PROGRAM;
    }
    return check_duration(vm, f);
}

/* ------------------------------------------------------------------------
 * Date arithmetic in the ISO calendar, and the difference of two dates or
 * date-times rounded as the 2024 revision of the specification rounds it
 * (the "nudge" of RoundRelativeDuration), as both polyfills compute it.
 */

static sjs_iso_date_time date_at(int64_t days) {
    sjs_iso_date_time d = { 0 };
    date_of_days(days, &d);
    return d;
}

/* The epoch nanoseconds of a date (days since the epoch) at a time of day. */
static wide epoch_nanoseconds(int64_t days, int64_t time) {
    return wide_add(wide_mul(wide_of(days), NS_PER_DAY), wide_of(time));
}

static int64_t days_of(const sjs_iso_date_time *d) {
    return epoch_days(d->year, d->month, d->day);
}

/* CalendarDateAdd: the years and months added, the month balanced into
 * the year, the day constrained to the month's last or, with `reject`,
 * faulting past it; then the weeks and days. Into `*out` (days since the
 * epoch); faults outside Temporal's limits. */
typedef struct {
    const sjs_iso_date_time *date;
    date_duration duration;
    bool reject;
} date_addition;

static sjs_result add_date(sjs_vm *vm, date_addition a, int64_t *out) {
    int64_t month = (int64_t)a.date->month - 1 + a.duration.months;
    int64_t year = (int64_t)a.date->year + a.duration.years + floor_div(month, 12);
    uint32_t m = (uint32_t)floor_mod(month, 12) + 1u;
    uint32_t day = a.date->day;
    uint32_t last = days_in_month(year, m);
    if (day > last) {
        if (a.reject) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a day past its month's last with overflow \"reject\"");
        day = last;
    }
    int64_t days = epoch_days(year, m, day) + a.duration.days + 7 * a.duration.weeks;
    if (days < DAYS_MIN || days > DAYS_MAX) return outside_limits(vm);
    *out = days;
    return SJS_OK;
}

/* Is the date y-m-d (d past the month's end, perhaps) past `two`, in the
 * direction of `sign`? (ISODateSurpasses) */
typedef struct {
    int64_t year;
    int64_t month;
    int64_t day;
} loose_date;

static bool surpasses(int sign, loose_date a, const sjs_iso_date_time *two) {
    int64_t delta = a.year != two->year ? a.year - two->year : a.month != two->month ? a.month - two->month : a.day - two->day;
    return sign * delta > 0;
}

/* The year and month `months` after `year`-`month`. */
static loose_date months_after(loose_date d, int64_t months) {
    int64_t month = d.month - 1 + months;
    return (loose_date){ d.year + floor_div(month, 12), floor_mod(month, 12) + 1, d.day };
}

/* CalendarDateUntil: from `one` to `two` in years, months, weeks and days,
 * as large as `largest` allows (a day at least). */
static date_duration date_until(const sjs_iso_date_time *one, const sjs_iso_date_time *two, time_unit largest) {
    date_duration r = { 0, 0, 0, 0 };
    int sign = -compare_dates(one, two);
    if (sign == 0) return r;
    loose_date start = { one->year, one->month, one->day };
    if (largest == UNIT_YEAR || largest == UNIT_MONTH) {
        int64_t candidate = (int64_t)two->year - one->year;
        if (candidate != 0) candidate -= sign;
        while (!surpasses(sign, (loose_date){ one->year + candidate, one->month, one->day }, two)) {
            r.years = candidate;
            candidate += sign;
        }
        int64_t months = sign;
        loose_date at = months_after((loose_date){ one->year + r.years, one->month, one->day }, months);
        while (!surpasses(sign, at, two)) {
            r.months = months;
            months += sign;
            at = months_after(at, sign);
        }
        if (largest == UNIT_MONTH) {
            r.months += r.years * 12;
            r.years = 0;
        }
    }
    loose_date at = months_after((loose_date){ start.year + r.years, start.month, start.day }, r.months);
    uint32_t last = days_in_month(at.year, (uint32_t)at.month);
    r.days = days_of(two) - epoch_days(at.year, (uint32_t)at.month, at.day > last ? last : (uint32_t)at.day);
    if (largest == UNIT_WEEK) {
        r.weeks = r.days / 7;
        r.days %= 7;
    }
    return r;
}

/* DifferenceISODateTime: from `a` to `b`, the date part as large as
 * `largest` (the days in the time when it is below a day). */
typedef struct {
    const sjs_iso_date_time *from;
    const sjs_iso_date_time *to;
} date_time_pair;

static internal_duration difference_iso(date_time_pair p, time_unit largest) {
    int64_t time = time_of_day(p.to) - time_of_day(p.from);
    int time_sign = time < 0 ? -1 : time > 0 ? 1 : 0;
    sjs_iso_date_time adjusted = date_at(days_of(p.to));
    if (compare_dates(p.from, p.to) == time_sign && time_sign != 0) {
        adjusted = date_at(days_of(p.to) + time_sign);
        time -= time_sign * NS_PER_DAY;
    }
    internal_duration d = { date_until(p.from, &adjusted, largest > UNIT_DAY ? largest : UNIT_DAY), wide_of(time) };
    if (largest < UNIT_DAY) {
        d.time = wide_add(d.time, wide_mul(wide_of(d.date.days), NS_PER_DAY));
        d.date.days = 0;
    }
    return d;
}

/* How to round a difference: its units, increment and mode. */
typedef struct {
    time_unit largest;
    time_unit smallest;
    int64_t increment;
    rounding_mode mode;
} difference_settings;

/* A difference to round, from `origin` (a date and time) to `dest` (its
 * epoch nanoseconds). */
typedef struct {
    internal_duration duration;
    wide dest;
    const sjs_iso_date_time *origin;
    difference_settings s;
} relative_rounding;

typedef struct {
    internal_duration duration;
    wide nudged;   /* the destination moved by the rounding */
    bool expanded;
    double total;
} nudge;

/* The epoch nanoseconds of an origin's date plus `duration`
 * (constrained), at the origin's time. */
typedef struct {
    const sjs_iso_date_time *origin;
    date_duration duration;
} origin_shift;

static sjs_result epoch_after(sjs_vm *vm, origin_shift shift, wide *out) {
    int64_t days;
    if (add_date(vm, (date_addition){ shift.origin, shift.duration, false }, &days) != SJS_OK) return SJS_ERR_PROGRAM;
    *out = epoch_nanoseconds(days, time_of_day(shift.origin));
    return SJS_OK;
}

/* NudgeToCalendarUnit: the smallest unit (a day to a year) truncated to
 * the increment, and the span from it to the next multiple; the
 * destination's place in the span rounds, and gives the total. */
static sjs_result nudge_calendar(sjs_vm *vm, const relative_rounding *r, nudge *out) {
    const date_duration *d = &r->duration.date;
    int64_t sign = internal_sign(&r->duration) < 0 ? -1 : 1;
    int64_t increment = r->s.increment;
    date_duration start = { 0, 0, 0, 0 };
    int64_t r1;
    switch (r->s.smallest) {
    case UNIT_YEAR:
        r1 = round_number(d->years, increment, MODE_TRUNC);
        start.years = r1;
        break;
    case UNIT_MONTH:
        r1 = round_number(d->months, increment, MODE_TRUNC);
        start = (date_duration){ d->years, r1, 0, 0 };
        break;
    case UNIT_WEEK: {
        int64_t weeks_start;
        if (add_date(vm, (date_addition){ r->origin, { d->years, d->months, 0, 0 }, false }, &weeks_start) != SJS_OK) return SJS_ERR_PROGRAM;
        sjs_iso_date_time from = date_at(weeks_start);
        sjs_iso_date_time to = date_at(weeks_start + d->days);
        r1 = round_number(d->weeks + date_until(&from, &to, UNIT_WEEK).weeks, increment, MODE_TRUNC);
        start = (date_duration){ d->years, d->months, r1, 0 };
        break;
    }
    default:
        r1 = round_number(d->days, increment, MODE_TRUNC);
        start = (date_duration){ d->years, d->months, d->weeks, r1 };
        break;
    }
    int64_t r2 = r1 + increment * sign;
    date_duration end = start;
    int64_t *field = r->s.smallest == UNIT_YEAR ? &end.years : r->s.smallest == UNIT_MONTH ? &end.months : r->s.smallest == UNIT_WEEK ? &end.weeks : &end.days;
    *field = r2;
    wide start_ns;
    wide end_ns;
    if (epoch_after(vm, (origin_shift){ r->origin, start }, &start_ns) != SJS_OK || epoch_after(vm, (origin_shift){ r->origin, end }, &end_ns) != SJS_OK) {
        return SJS_ERR_PROGRAM;
    }
    /* Outside the window from start to end, the specification now moves
     * it on by an increment (ComputeNudgeWindow's additionalShift), where
     * @js-temporal/polyfill and staticjs followed the older text, which
     * rounds such a destination to the start: a month's end counted into
     * a shorter month (P1MT12H from 2024-01-31 in months). The polyfills
     * disagree there, and the reference polyfill bubbles the result
     * differently again, so staticjs faults rather than pick one. */
    bool outside = sign > 0 ? wide_cmp(r->dest, start_ns) < 0 || wide_cmp(r->dest, end_ns) > 0
                            : wide_cmp(r->dest, end_ns) < 0 || wide_cmp(r->dest, start_ns) > 0;
    if (outside) return sjs_vm_fault(vm, "staticjs does not round to a calendar unit where the destination is past the window the duration gives (from a month's end into a shorter month): the specification moves the window on (ComputeNudgeWindow)");
    wide numerator = wide_sub(r->dest, start_ns);
    wide denominator = wide_sub(end_ns, start_ns);
    bool expanded;
    if (wide_sign(numerator) == 0) {
        expanded = false;
    } else if (wide_cmp(numerator, denominator) == 0) {
        expanded = true;
    } else {
        int cmp = u128_cmp(u128_add(numerator.m, numerator.m), denominator.m);
        bool even = ((r1 < 0 ? -r1 : r1) / increment) % 2 == 0;
        expanded = rounds_away((rounding_case){ r->s.mode, sign < 0, cmp, even });
    }
    out->total = ratio_double(wide_add(wide_mul(denominator, r1), wide_mul(numerator, increment * sign)), denominator);
    out->duration = (internal_duration){ expanded ? end : start, wide_of(0) };
    out->nudged = expanded ? end_ns : start_ns;
    out->expanded = expanded;
    return SJS_OK;
}

/* NudgeToDayOrTime: the days and time as nanoseconds, rounded; whether
 * that crossed into another day. */
static nudge nudge_time(const relative_rounding *r) {
    const internal_duration *d = &r->duration;
    wide time = wide_add(d->time, wide_mul(wide_of(d->date.days), NS_PER_DAY));
    u128 increment = u128_mul((u128){ 0, (uint64_t)UNIT_NANOSECONDS[r->s.smallest] }, (uint64_t)r->s.increment);
    wide rounded = round_wide(time, (wide_rounding){ increment, r->s.mode });
    wide rest;
    int64_t whole_days = wide_int64(wide_quotient(time, (u128){ 0, (uint64_t)NS_PER_DAY }, &rest));
    int64_t rounded_days = wide_int64(wide_quotient(rounded, (u128){ 0, (uint64_t)NS_PER_DAY }, &rest));
    int64_t delta = rounded_days - whole_days;
    nudge n = { *d, wide_add(r->dest, wide_sub(rounded, time)), (delta < 0 ? -1 : delta > 0 ? 1 : 0) == wide_sign(time), 0 };
    n.duration.date.days = 0;
    n.duration.time = rounded;
    if (r->s.largest >= UNIT_DAY) {
        n.duration.date.days = rounded_days;
        n.duration.time = rest;
    }
    return n;
}

/* BubbleRelativeDuration: a rounding that reached the end of a larger unit
 * (a week only when it is the largest) carries into it, up to `largest`. */
static sjs_result bubble(sjs_vm *vm, const relative_rounding *r, nudge *n) {
    int64_t sign = internal_sign(&r->duration) < 0 ? -1 : 1;
    time_unit from = r->s.smallest > UNIT_DAY ? r->s.smallest : UNIT_DAY;
    if (from == r->s.largest) return SJS_OK;
    for (uint32_t unit = from + 1u; unit <= r->s.largest; unit++) {
        if (unit == UNIT_WEEK && r->s.largest != UNIT_WEEK) continue;
        date_duration end = n->duration.date;
        if (unit == UNIT_YEAR) end = (date_duration){ end.years + sign, 0, 0, 0 };
        if (unit == UNIT_MONTH) end = (date_duration){ end.years, end.months + sign, 0, 0 };
        if (unit == UNIT_WEEK) end = (date_duration){ end.years, end.months, end.weeks + sign, 0 };
        wide end_ns;
        if (epoch_after(vm, (origin_shift){ r->origin, end }, &end_ns) != SJS_OK) return SJS_ERR_PROGRAM;
        if (wide_cmp(n->nudged, end_ns) == -sign) break;
        n->duration = (internal_duration){ end, wide_of(0) };
    }
    return SJS_OK;
}

/* RoundRelativeDuration. */
static sjs_result round_relative(sjs_vm *vm, const relative_rounding *r, internal_duration *out) {
    nudge n;
    if (is_calendar_unit(r->s.smallest)) {
        if (nudge_calendar(vm, r, &n) != SJS_OK) return SJS_ERR_PROGRAM;
    } else {
        n = nudge_time(r);
    }
    if (n.expanded && r->s.smallest != UNIT_WEEK && bubble(vm, r, &n) != SJS_OK) return SJS_ERR_PROGRAM;
    *out = n.duration;
    return SJS_OK;
}

/* DifferencePlainDateTimeWithRounding: zero for equal date-times; each
 * within Temporal's limits. */
typedef struct {
    date_time_pair pair;
    difference_settings s;
} rounded_difference;

static sjs_result difference_rounded(sjs_vm *vm, rounded_difference d, internal_duration *out) {
    date_time_pair p = d.pair;
    difference_settings s = d.s;
    *out = (internal_duration){ { 0, 0, 0, 0 }, wide_of(0) };
    if (compare_date_times(p.from, p.to) == 0) return SJS_OK;
    if (!within_limits(p.from, true) || !within_limits(p.to, true)) return outside_limits(vm);
    *out = difference_iso(p, s.largest);
    if (s.smallest == UNIT_NANOSECOND && s.increment == 1) return SJS_OK;
    relative_rounding r = { *out, epoch_nanoseconds(days_of(p.to), time_of_day(p.to)), p.from, s };
    return round_relative(vm, &r, out);
}

/* DifferencePlainDateTimeWithTotal, in `unit`. */
typedef struct {
    date_time_pair pair;
    time_unit unit;
} total_difference;

static sjs_result difference_total(sjs_vm *vm, total_difference t, double *out) {
    date_time_pair p = t.pair;
    time_unit unit = t.unit;
    *out = 0;
    if (compare_date_times(p.from, p.to) == 0) return SJS_OK;
    if (!within_limits(p.from, true) || !within_limits(p.to, true)) return outside_limits(vm);
    internal_duration d = difference_iso(p, unit);
    if (is_calendar_unit(unit)) {
        relative_rounding r = { d, epoch_nanoseconds(days_of(p.to), time_of_day(p.to)), p.from, { unit, unit, 1, MODE_TRUNC } };
        nudge n;
        if (nudge_calendar(vm, &r, &n) != SJS_OK) return SJS_ERR_PROGRAM;
        *out = n.total;
        return SJS_OK;
    }
    wide time = wide_add(d.time, wide_mul(wide_of(d.date.days), NS_PER_DAY));
    *out = ratio_double(time, wide_of(UNIT_NANOSECONDS[unit]));
    return SJS_OK;
}

/* A unit option (largestUnit, smallestUnit, unit): a unit's name from a
 * string, in `range`, or "auto" where `automatic`; UNIT_NONE when it is
 * undefined. Any other value faults (a RangeError, or a conversion). */
#define UNIT_NONE ((uint32_t)UNIT_COUNT)
#define UNIT_AUTO ((uint32_t)UNIT_COUNT + 1u)

typedef struct {
    sjs_value options;
    const char *name;
    unit_range range;
    bool automatic;
} unit_request;

static sjs_result unit_value(sjs_vm *vm, unit_request request, uint32_t *out) {
    sjs_value v;
    *out = UNIT_NONE;
    if (option_of(vm, (option_name){ request.options, sjs_cstr(request.name) }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    if (v.kind == SJS_UNDEFINED) return SJS_OK;
    if (v.kind != SJS_STRING) return sjs_vm_fault(vm, "staticjs reads a Temporal option only from a string, where JavaScript would convert this value to one");
    sjs_str text = sjs_string_view(SJS_AS_STRING(v));
    if (request.automatic && sjs_str_equal(text, SJS_STR("auto"))) {
        *out = UNIT_AUTO;
        return SJS_OK;
    }
    time_unit unit;
    if (unit_in_range(vm, (unit_text){ text, request.range }, &unit) != SJS_OK) return SJS_ERR_PROGRAM;
    *out = unit;
    return SJS_OK;
}

static rounding_mode negated_mode(rounding_mode mode) {
    switch (mode) {
    case MODE_CEIL:
        return MODE_FLOOR;
    case MODE_FLOOR:
        return MODE_CEIL;
    case MODE_HALF_CEIL:
        return MODE_HALF_FLOOR;
    case MODE_HALF_FLOOR:
        return MODE_HALF_CEIL;
    default:
        return mode;
    }
}

/* GetDifferenceSettings of until and since: the units of the receiver's
 * type (a date's day to year, a time's and an Instant's nanosecond to hour,
 * both for a date and time), smallestUnit by default the least of them but
 * a date's day, largestUnit by default ("auto") the larger of smallestUnit
 * and a day (an hour for a time, a second for an Instant); roundingMode trunc by default, its direction negated
 * for since; an increment of a time unit divides the unit above. */
typedef struct {
    sjs_value options;
    sjs_temporal_type type;
    bool since;
} difference_request;

static sjs_result difference_options(sjs_vm *vm, difference_request request, difference_settings *out) {
    sjs_temporal_type type = request.type;
    bool timed = type == SJS_TEMPORAL_PLAIN_TIME || type == SJS_TEMPORAL_INSTANT;
    unit_range range = { type == SJS_TEMPORAL_PLAIN_DATE ? UNIT_DAY : UNIT_NANOSECOND, timed ? UNIT_HOUR : UNIT_YEAR };
    time_unit fallback = type == SJS_TEMPORAL_PLAIN_DATE ? UNIT_DAY : UNIT_NANOSECOND;
    time_unit default_largest = type == SJS_TEMPORAL_INSTANT ? UNIT_SECOND : type == SJS_TEMPORAL_PLAIN_TIME ? UNIT_HOUR : UNIT_DAY;
    uint32_t largest;
    uint32_t smallest;
    *out = (difference_settings){ UNIT_DAY, fallback, 1, MODE_TRUNC };
    if (unit_value(vm, (unit_request){ request.options, "largestUnit", range, true }, &largest) != SJS_OK) return SJS_ERR_PROGRAM;
    if (increment_option(vm, request.options, &out->increment) != SJS_OK || mode_option(vm, request.options, &out->mode) != SJS_OK) return SJS_ERR_PROGRAM;
    if (request.since) out->mode = negated_mode(out->mode);
    if (unit_value(vm, (unit_request){ request.options, "smallestUnit", range, false }, &smallest) != SJS_OK) return SJS_ERR_PROGRAM;
    if (smallest != UNIT_NONE) out->smallest = (time_unit)smallest;
    time_unit larger = out->smallest > default_largest ? out->smallest : default_largest;
    out->largest = largest == UNIT_NONE || largest == UNIT_AUTO ? larger : (time_unit)largest;
    if (out->largest < out->smallest) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a largestUnit smaller than the smallestUnit");
    if (out->smallest < UNIT_DAY) return check_increment(vm, out->increment, out->smallest);
    return SJS_OK;
}

/* relativeTo: a PlainDate's date, or a PlainDateTime's, a string's or an
 * object's fields' (with their time, which is read but not used);
 * `*given` false when it is undefined. JavaScript reads a time zone, an
 * offset with one or a time zone annotation as a ZonedDateTime, which
 * staticjs does not have. */
typedef struct {
    sjs_iso_date_time date;
    bool given;
} relative_date;

static sjs_result zoned_relative_to(sjs_vm *vm) {
    return sjs_vm_fault(vm, "staticjs has no Temporal.ZonedDateTime: a relativeTo with a time zone needs time zone data");
}

static sjs_result relative_to_option(sjs_vm *vm, sjs_value options, relative_date *out) {
    sjs_value v;
    out->given = false;
    if (option_of(vm, (option_name){ options, SJS_STR("relativeTo") }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    if (v.kind == SJS_UNDEFINED) return SJS_OK;
    out->given = true;
    if (v.kind == SJS_STRING) {
        sjs_str text = parsed_text(vm, v);
        iso_parse parsed;
        if (parse_iso(text, &parsed) && parsed.zoned) return zoned_relative_to(vm);
    } else if (v.kind == SJS_OBJECT) {
        const sjs_object *bag = SJS_AS_OBJECT(v);
        const sjs_entry *zone = sjs_object_find(bag, SJS_STR("timeZone"));
        const sjs_entry *offset = sjs_object_find(bag, SJS_STR("offset"));
        if ((zone != NULL && zone->value.kind != SJS_UNDEFINED) || (offset != NULL && offset->value.kind != SJS_UNDEFINED)) return zoned_relative_to(vm);
        time_fields g = { 0 };
        if (read_time_fields(vm, bag, &g) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return to_date(vm, (date_source){ v, sjs_undefined() }, &out->date);
}

/* ------------------------------------------------------------------------
 * A Duration's functions and methods
 */

/* new Temporal.Duration(years, ..., nanoseconds): each undefined is 0. */
static sjs_result construct_duration(sjs_vm *vm, sjs_leaf_call *call) {
    double f[DURATION_FIELD_COUNT];
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) {
        sjs_value v = arg_of(call, i);
        f[i] = 0;
        if (v.kind != SJS_UNDEFINED && integral_of(vm, v, &f[i]) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return new_duration(vm, f, &call->result);
}

/* The days the years, months and weeks of `d` span from `date`, and its
 * days (DateDurationDays). */
typedef struct {
    const date_duration *duration;
    const sjs_iso_date_time *date;
} dated_duration;

static sjs_result days_from(sjs_vm *vm, dated_duration from, int64_t *out) {
    const date_duration *d = from.duration;
    const sjs_iso_date_time *date = from.date;
    *out = d->days;
    if (d->years == 0 && d->months == 0 && d->weeks == 0) return SJS_OK;
    int64_t later;
    if (add_date(vm, (date_addition){ date, { d->years, d->months, d->weeks, 0 }, false }, &later) != SJS_OK) return SJS_ERR_PROGRAM;
    *out += later - days_of(date);
    return SJS_OK;
}

/* Temporal.Duration.compare(one, two, { relativeTo }): equal fields are 0;
 * years, months or weeks in either need relativeTo, from which they count
 * in days; a day is 24 hours. */
static sjs_result compare_durations(sjs_vm *vm, sjs_leaf_call *call) {
    double a[DURATION_FIELD_COUNT];
    double b[DURATION_FIELD_COUNT];
    relative_date relative;
    if (to_duration(vm, arg_of(call, 0), a) != SJS_OK || to_duration(vm, arg_of(call, 1u), b) != SJS_OK) return SJS_ERR_PROGRAM;
    if (relative_to_option(vm, arg_of(call, 2u), &relative) != SJS_OK) return SJS_ERR_PROGRAM;
    call->result = sjs_num(0);
    uint32_t i = 0;
    while (i < DURATION_FIELD_COUNT && a[i] == b[i]) i++;
    if (i == DURATION_FIELD_COUNT) return SJS_OK;
    internal_duration x = internal_of(a, false);
    internal_duration y = internal_of(b, false);
    int64_t x_days = x.date.days;
    int64_t y_days = y.date.days;
    if (is_calendar_unit(largest_unit_of(a)) || is_calendar_unit(largest_unit_of(b))) {
        if (!relative.given) return sjs_vm_fault(vm, "JavaScript throws a RangeError for comparing years, months or weeks without relativeTo");
        if (days_from(vm, (dated_duration){ &x.date, &relative.date }, &x_days) != SJS_OK ||
            days_from(vm, (dated_duration){ &y.date, &relative.date }, &y_days) != SJS_OK) {
            return SJS_ERR_PROGRAM;
        }
    }
    wide x_time = wide_add(x.time, wide_mul(wide_of(x_days), NS_PER_DAY));
    wide y_time = wide_add(y.time, wide_mul(wide_of(y_days), NS_PER_DAY));
    call->result = sjs_num((double)wide_cmp(x_time, y_time));
    return SJS_OK;
}


/* A Duration's getter. */
static sjs_value duration_field(const double f[DURATION_FIELD_COUNT], sjs_temporal_field field) {
    switch (field) {
    case SJS_FIELD_SIGN:
        return sjs_num((double)duration_sign(f));
    case SJS_FIELD_BLANK:
        return sjs_boolean(duration_sign(f) == 0);
    case SJS_FIELD_YEARS:
        return sjs_num(f[0]);
    case SJS_FIELD_MONTHS:
        return sjs_num(f[1]);
    case SJS_FIELD_WEEKS:
        return sjs_num(f[2]);
    case SJS_FIELD_DAYS:
        return sjs_num(f[3]);
    case SJS_FIELD_HOURS:
        return sjs_num(f[4]);
    case SJS_FIELD_MINUTES:
        return sjs_num(f[5]);
    case SJS_FIELD_SECONDS:
        return sjs_num(f[6]);
    case SJS_FIELD_MILLISECONDS:
        return sjs_num(f[7]);
    case SJS_FIELD_MICROSECONDS:
        return sjs_num(f[8]);
    default:
        return sjs_num(f[9]);
    }
}


static sjs_result instant_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
static sjs_result now_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);

sjs_result sjs_temporal_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    static const struct {
        sjs_builtin_id id;
        sjs_temporal_type type;
    } TYPES[] = {
        { SJS_BUILTIN_TEMPORAL_PLAIN_DATE, SJS_TEMPORAL_PLAIN_DATE },
        { SJS_BUILTIN_TEMPORAL_PLAIN_TIME, SJS_TEMPORAL_PLAIN_TIME },
        { SJS_BUILTIN_TEMPORAL_PLAIN_DATE_TIME, SJS_TEMPORAL_PLAIN_DATE_TIME },
    };
    switch (id) {
    case SJS_BUILTIN_TEMPORAL_DURATION:
        return construct_duration(vm, call);
    case SJS_BUILTIN_TEMPORAL_DURATION_FROM: {
        double f[DURATION_FIELD_COUNT];
        sjs_value item = arg_of(call, 0);
        if (to_duration(vm, item, f) != SJS_OK) return SJS_ERR_PROGRAM;
        if (!is_temporal(item, SJS_TEMPORAL_DURATION)) return new_duration(vm, f, &call->result);
        call->result = item;
        sjs_ref(item);
        return SJS_OK;
    }
    case SJS_BUILTIN_TEMPORAL_DURATION_COMPARE:
        return compare_durations(vm, call);
    case SJS_BUILTIN_TEMPORAL_INSTANT:
    case SJS_BUILTIN_TEMPORAL_INSTANT_FROM:
    case SJS_BUILTIN_TEMPORAL_INSTANT_COMPARE:
    case SJS_BUILTIN_TEMPORAL_INSTANT_FROM_EPOCH_MILLISECONDS:
    case SJS_BUILTIN_TEMPORAL_INSTANT_FROM_EPOCH_NANOSECONDS:
        return instant_function(vm, id, call);
    case SJS_BUILTIN_TEMPORAL_NOW_INSTANT:
    case SJS_BUILTIN_TEMPORAL_NOW_PLAIN_DATE_ISO:
    case SJS_BUILTIN_TEMPORAL_NOW_PLAIN_DATE_TIME_ISO:
    case SJS_BUILTIN_TEMPORAL_NOW_PLAIN_TIME_ISO:
    case SJS_BUILTIN_TEMPORAL_NOW_TIME_ZONE_ID:
    case SJS_BUILTIN_TEMPORAL_NOW_ZONED_DATE_TIME_ISO:
        return now_function(vm, id, call);
    default:
        break;
    }
    /* Each plain type's constructor, from and compare are three IDs in a row. */
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
#define A_DATE TYPE_BIT(SJS_TEMPORAL_DATE)
/* Temporal's five types; a Date has none of their getters, and of their
 * methods' names only toJSON, toLocaleString, toString and valueOf. */
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
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_JSON)] = ALL_TYPES | A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_LOCALE_STRING)] = ALL_TYPES | A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_DATE)] = PLAIN_DATE_TIME,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_DATE_TIME)] = PLAIN_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_MONTH_DAY)] = PLAIN_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_TIME)] = PLAIN_DATE_TIME,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_PLAIN_YEAR_MONTH)] = PLAIN_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_STRING)] = ALL_TYPES | A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME)] = DATED,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME_ISO)] = INSTANT,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_TOTAL)] = DURATION,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_UNTIL)] = PLAIN_DATE | PLAIN_TIME | PLAIN_DATE_TIME | INSTANT,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_VALUE_OF)] = ALL_TYPES | A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_WITH)] = PLAIN_DATE | PLAIN_TIME | PLAIN_DATE_TIME | DURATION,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_WITH_CALENDAR)] = DATED,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_GET_TIME)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_GET_UTC_DATE)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_GET_UTC_DAY)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_GET_UTC_FULL_YEAR)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_GET_UTC_HOURS)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_GET_UTC_MILLISECONDS)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_GET_UTC_MINUTES)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_GET_UTC_MONTH)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_GET_UTC_SECONDS)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_TO_ISO_STRING)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_TO_UTC_STRING)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_LOCAL)] = A_DATE,
    [METHOD_INDEX(SJS_METHOD_TEMPORAL_DATE_SETTER)] = A_DATE,
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

/* An Instant's getter (below). */
typedef struct {
    const sjs_temporal *instant;
    sjs_temporal_field field;
} instant_getter;

static sjs_result instant_field(sjs_vm *vm, instant_getter getter, sjs_value *out);

sjs_result sjs_temporal_property(sjs_vm *vm, sjs_member member, sjs_value *out) {
    sjs_value v = member.object;
    const sjs_name *n = sjs_name_of(member.key);
    *out = sjs_undefined();
    sjs_temporal_field field = temporal_field_of(n, v);
    if (field != SJS_FIELD_NONE) {
        sjs_temporal_type type = sjs_temporal_type_of(v);
        vm->fuel_used++;
        if (type == SJS_TEMPORAL_DURATION) {
            *out = duration_field(SJS_AS_TEMPORAL(v)->as.duration, field);
            return SJS_OK;
        }
        if (type == SJS_TEMPORAL_INSTANT) return instant_field(vm, (instant_getter){ SJS_AS_TEMPORAL(v), field }, out);
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
 * roundingMode }) of a PlainTime, a PlainDateTime or an Instant: the unit
 * in `range`, the increment as given (the caller checks it), roundingMode
 * halfExpand by default. smallestUnit is required (a TypeError without
 * options, a RangeError without the unit). */
typedef struct {
    time_unit unit;
    rounding r;
} round_choice;

static sjs_result round_options(sjs_vm *vm, unit_option request, round_choice *out) {
    sjs_value options = request.options;
    unit_choice unit = { UNIT_NANOSECOND, true };
    out->r = (rounding){ 1, MODE_HALF_EXPAND };
    if (options.kind == SJS_STRING) return unit_in_range(vm, (unit_text){ sjs_string_view(SJS_AS_STRING(options)), request.range }, &out->unit);
    if (options.kind == SJS_UNDEFINED) return sjs_vm_fault(vm, "JavaScript throws a TypeError for round without a smallestUnit");
    if (increment_option(vm, options, &out->r.increment) != SJS_OK || mode_option(vm, options, &out->r.mode) != SJS_OK) return SJS_ERR_PROGRAM;
    if (smallest_unit(vm, request, &unit) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!unit.given) return sjs_vm_fault(vm, "JavaScript throws a RangeError for round's options without a smallestUnit");
    out->unit = unit.unit;
    return SJS_OK;
}

/* round: a PlainTime to an hour at most, a PlainDateTime to a day. */
static sjs_result iso_round(sjs_vm *vm, const sjs_temporal *t, sjs_leaf_call *call) {
    sjs_temporal_type type = (sjs_temporal_type)t->h.flags;
    unit_range range = { UNIT_NANOSECOND, type == SJS_TEMPORAL_PLAIN_TIME ? UNIT_HOUR : UNIT_DAY };
    round_choice choice;
    if (round_options(vm, (unit_option){ arg_of(call, 0), range }, &choice) != SJS_OK) return SJS_ERR_PROGRAM;
    if (check_increment(vm, choice.r.increment, choice.unit) != SJS_OK) return SJS_ERR_PROGRAM;
    rounding r = { choice.r.increment * UNIT_NANOSECONDS[choice.unit], choice.r.mode };
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

/* The options, and the largest smallestUnit: a minute, or a Duration's
 * second. */
typedef struct {
    sjs_value options;
    time_unit most;
} precision_request;

static sjs_result precision_of(sjs_vm *vm, precision_request request, text_precision *out) {
    sjs_value options = request.options;
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
    if (smallest_unit(vm, (unit_option){ options, { UNIT_NANOSECOND, request.most } }, &unit) != SJS_OK) return SJS_ERR_PROGRAM;
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
        if (precision_of(vm, (precision_request){ request.options, UNIT_MINUTE }, &precision) != SJS_OK) return SJS_ERR_PROGRAM;
        if (!round_iso(&d, precision.r, type == SJS_TEMPORAL_PLAIN_DATE_TIME)) return outside_limits(vm);
    }
    char text[SJS_TEMPORAL_TEXT_MAX + 16u];
    uint32_t n = put_iso(text, (iso_value){ type, &d }, precision.digits);
    sjs_str suffix = choice == 2u ? SJS_STR("[u-ca=iso8601]") : choice == 3u ? SJS_STR("[!u-ca=iso8601]") : SJS_STR("");
    sjs_copy(text + n, suffix.ptr, suffix.length);
    vm->fuel_used += n + suffix.length;
    return temporal_string(vm, (sjs_str){ text, n + suffix.length }, out);
}

/* The date and time a Duration (its days as 24 hours) reaches from
 * relativeTo's date at midnight, the years, months and weeks added to it
 * constrained, into `to`. */
typedef struct {
    const double *fields;
    const relative_date *relative;
} relative_duration;

static sjs_result reached(sjs_vm *vm, relative_duration r, sjs_iso_date_time *to) {
    const relative_date *relative = r.relative;
    internal_duration d = internal_of(r.fields, true);
    wide time;
    int64_t days = wide_int64(wide_floor(d.time, (u128){ 0, (uint64_t)NS_PER_DAY }, &time));
    int64_t target;
    if (add_date(vm, (date_addition){ &relative->date, { d.date.years, d.date.months, d.date.weeks, days }, false }, &target) != SJS_OK) return SJS_ERR_PROGRAM;
    *to = date_at(target);
    set_time_of_day(to, wide_int64(time));
    return SJS_OK;
}

static sjs_result needs_relative_to(sjs_vm *vm) {
    return sjs_vm_fault(vm, "JavaScript throws a RangeError for years, months or weeks without relativeTo");
}

static const unit_range ALL_UNITS = { UNIT_NANOSECOND, UNIT_YEAR };

/* round(smallestUnit) or round({ largestUnit, relativeTo,
 * roundingIncrement, roundingMode, smallestUnit }): one of the units at
 * least; largestUnit by default the larger of smallestUnit and the
 * Duration's largest; roundingMode halfExpand. Relative to a date, as the
 * difference from it to where the Duration reaches; without one, of 24-hour
 * days and time, and no years, months or weeks. */
static sjs_result duration_round(sjs_vm *vm, const double f[DURATION_FIELD_COUNT], sjs_leaf_call *call) {
    sjs_value options = arg_of(call, 0);
    uint32_t largest = UNIT_NONE;
    uint32_t smallest = UNIT_NONE;
    relative_date relative = { { 0 }, false };
    difference_settings s = { UNIT_NANOSECOND, UNIT_NANOSECOND, 1, MODE_HALF_EXPAND };
    if (options.kind == SJS_UNDEFINED) return sjs_vm_fault(vm, "JavaScript throws a TypeError for round without options");
    if (options.kind == SJS_STRING) {
        time_unit unit;
        if (unit_in_range(vm, (unit_text){ sjs_string_view(SJS_AS_STRING(options)), ALL_UNITS }, &unit) != SJS_OK) return SJS_ERR_PROGRAM;
        smallest = unit;
    } else {
        if (unit_value(vm, (unit_request){ options, "largestUnit", ALL_UNITS, true }, &largest) != SJS_OK) return SJS_ERR_PROGRAM;
        if (relative_to_option(vm, options, &relative) != SJS_OK) return SJS_ERR_PROGRAM;
        if (increment_option(vm, options, &s.increment) != SJS_OK || mode_option(vm, options, &s.mode) != SJS_OK) return SJS_ERR_PROGRAM;
        if (unit_value(vm, (unit_request){ options, "smallestUnit", ALL_UNITS, false }, &smallest) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    if (smallest == UNIT_NONE && (largest == UNIT_NONE)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for round without a smallestUnit or a largestUnit");
    s.smallest = smallest == UNIT_NONE ? UNIT_NANOSECOND : (time_unit)smallest;
    time_unit existing = largest_unit_of(f);
    time_unit larger = existing > s.smallest ? existing : s.smallest;
    s.largest = largest == UNIT_NONE || largest == UNIT_AUTO ? larger : (time_unit)largest;
    if (s.largest < s.smallest) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a largestUnit smaller than the smallestUnit");
    if (s.smallest < UNIT_DAY && check_increment(vm, s.increment, s.smallest) != SJS_OK) return SJS_ERR_PROGRAM;
    if (s.increment > 1 && s.smallest >= UNIT_DAY && s.largest != s.smallest) {
        return sjs_vm_fault(vm, "JavaScript throws a RangeError for a roundingIncrement above 1 of a day or a calendar unit other than the largestUnit");
    }
    internal_duration d;
    if (relative.given) {
        sjs_iso_date_time to;
        if (reached(vm, (relative_duration){ f, &relative }, &to) != SJS_OK) return SJS_ERR_PROGRAM;
        if (difference_rounded(vm, (rounded_difference){ { &relative.date, &to }, s }, &d) != SJS_OK) return SJS_ERR_PROGRAM;
        return duration_of_internal(vm, (duration_balance){ d, s.largest }, &call->result);
    }
    if (is_calendar_unit(existing) || is_calendar_unit(s.largest)) return needs_relative_to(vm);
    d = internal_of(f, true);
    u128 increment = u128_mul((u128){ 0, (uint64_t)UNIT_NANOSECONDS[s.smallest] }, (uint64_t)s.increment);
    d.time = round_wide(d.time, (wide_rounding){ increment, s.mode });
    return duration_of_internal(vm, (duration_balance){ d, s.largest }, &call->result);
}

/* total(unit) or total({ relativeTo, unit }): the Duration in the unit, a
 * number; relative to a date as round is; else of 24-hour days. */
static sjs_result duration_total(sjs_vm *vm, const double f[DURATION_FIELD_COUNT], sjs_leaf_call *call) {
    sjs_value options = arg_of(call, 0);
    uint32_t unit = UNIT_NONE;
    relative_date relative = { { 0 }, false };
    if (options.kind == SJS_UNDEFINED) return sjs_vm_fault(vm, "JavaScript throws a TypeError for total without options");
    if (options.kind == SJS_STRING) {
        time_unit u;
        if (unit_in_range(vm, (unit_text){ sjs_string_view(SJS_AS_STRING(options)), ALL_UNITS }, &u) != SJS_OK) return SJS_ERR_PROGRAM;
        unit = u;
    } else {
        if (relative_to_option(vm, options, &relative) != SJS_OK) return SJS_ERR_PROGRAM;
        if (unit_value(vm, (unit_request){ options, "unit", ALL_UNITS, false }, &unit) != SJS_OK) return SJS_ERR_PROGRAM;
        if (unit == UNIT_NONE) return sjs_vm_fault(vm, "JavaScript throws a RangeError for total without a unit");
    }
    double total;
    if (relative.given) {
        sjs_iso_date_time to;
        if (reached(vm, (relative_duration){ f, &relative }, &to) != SJS_OK) return SJS_ERR_PROGRAM;
        if (difference_total(vm, (total_difference){ { &relative.date, &to }, (time_unit)unit }, &total) != SJS_OK) return SJS_ERR_PROGRAM;
    } else {
        if (is_calendar_unit(largest_unit_of(f)) || is_calendar_unit((time_unit)unit)) return needs_relative_to(vm);
        total = ratio_double(internal_of(f, true).time, wide_of(UNIT_NANOSECONDS[unit]));
    }
    call->result = sjs_num(total);
    return SJS_OK;
}

/* toString({ fractionalSecondDigits, roundingMode, smallestUnit }): the
 * seconds rounded (trunc by default) to the digits written, carried into
 * the larger units up to the Duration's largest. */
typedef struct {
    const double *fields;
    sjs_value options;
} duration_text;

static sjs_result duration_to_string(sjs_vm *vm, duration_text request, sjs_value *out) {
    const double *f = request.fields;
    text_precision precision;
    if (precision_of(vm, (precision_request){ request.options, UNIT_SECOND }, &precision) != SJS_OK) return SJS_ERR_PROGRAM;
    double g[DURATION_FIELD_COUNT];
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) g[i] = f[i];
    if (precision.r.increment != 1) {
        time_unit largest = largest_unit_of(f);
        internal_duration d = internal_of(f, false);
        d.time = round_wide(d.time, (wide_rounding){ { 0, (uint64_t)precision.r.increment }, precision.r.mode });
        fields_of_internal((duration_balance){ d, largest > UNIT_SECOND ? largest : UNIT_SECOND }, g);
        if (check_duration(vm, g) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    char text[SJS_TEMPORAL_TEXT_MAX];
    uint32_t n = put_duration(text, g, precision.digits);
    vm->fuel_used += n;
    return temporal_string(vm, (sjs_str){ text, n }, out);
}

/* add(other) and subtract(other): of 24-hour days and time only (years,
 * months or weeks in either fault: JavaScript throws a RangeError),
 * balanced up to the larger of the two's largest units. */
static sjs_result duration_add(sjs_vm *vm, sjs_leaf_call *call, bool subtract) {
    const double *f = SJS_AS_TEMPORAL(call->receiver)->as.duration;
    double g[DURATION_FIELD_COUNT];
    if (to_duration(vm, arg_of(call, 0), g) != SJS_OK) return SJS_ERR_PROGRAM;
    if (subtract) negate_fields(g);
    time_unit a = largest_unit_of(f);
    time_unit b = largest_unit_of(g);
    time_unit largest = a > b ? a : b;
    if (is_calendar_unit(largest)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for adding Durations with years, months or weeks");
    internal_duration d = { { 0, 0, 0, 0 }, wide_add(internal_of(f, true).time, internal_of(g, true).time) };
    return duration_of_internal(vm, (duration_balance){ d, largest }, &call->result);
}

static sjs_result duration_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    const double *f = SJS_AS_TEMPORAL(call->receiver)->as.duration;
    double g[DURATION_FIELD_COUNT];
    for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) g[i] = f[i];
    switch (id) {
    case SJS_METHOD_TEMPORAL_WITH:
        if (partial_duration(vm, arg_of(call, 0), g) != SJS_OK) return SJS_ERR_PROGRAM;
        return new_duration(vm, g, &call->result);
    case SJS_METHOD_TEMPORAL_NEGATED:
        negate_fields(g);
        return new_duration(vm, g, &call->result);
    case SJS_METHOD_TEMPORAL_ABS:
        for (uint32_t i = 0; i < DURATION_FIELD_COUNT; i++) g[i] = field_magnitude(g[i]);
        return new_duration(vm, g, &call->result);
    case SJS_METHOD_TEMPORAL_ADD:
    case SJS_METHOD_TEMPORAL_SUBTRACT:
        return duration_add(vm, call, id == SJS_METHOD_TEMPORAL_SUBTRACT);
    case SJS_METHOD_TEMPORAL_ROUND:
        return duration_round(vm, f, call);
    case SJS_METHOD_TEMPORAL_TOTAL:
        return duration_total(vm, f, call);
    case SJS_METHOD_TEMPORAL_TO_STRING:
        return duration_to_string(vm, (duration_text){ f, arg_of(call, 0) }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_JSON:
        return duration_to_string(vm, (duration_text){ f, sjs_undefined() }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_LOCALE_STRING:
        return sjs_vm_fault(vm, "toLocaleString depends on locale data, which staticjs does not have");
    default:
        return sjs_vm_fault(vm, "JavaScript throws a TypeError for valueOf of a Temporal value: compare with equals or compare");
    }
}

/* ------------------------------------------------------------------------
 * add, subtract, until and since of a PlainDate, a PlainTime and a
 * PlainDateTime
 */

/* add(duration, options) and subtract: a date adds the years and months
 * (the day constrained to the month, or rejected: overflow), then the
 * weeks and the days, with the time's whole days (toward zero); a time
 * adds the days and time, around the clock (the years, months and weeks
 * are not used); a date and time adds the time, then the date with the
 * days the time carried. */
static sjs_result iso_add(sjs_vm *vm, sjs_leaf_call *call, bool subtract) {
    const sjs_temporal *t = SJS_AS_TEMPORAL(call->receiver);
    sjs_temporal_type type = (sjs_temporal_type)t->h.flags;
    double f[DURATION_FIELD_COUNT];
    if (to_duration(vm, arg_of(call, 0), f) != SJS_OK) return SJS_ERR_PROGRAM;
    if (subtract) negate_fields(f);
    internal_duration d = internal_of(f, true);
    sjs_iso_date_time made = t->as.iso;
    wide rest;
    if (type == SJS_TEMPORAL_PLAIN_TIME) {
        wide_floor(wide_add(d.time, wide_of(time_of_day(&made))), (u128){ 0, (uint64_t)NS_PER_DAY }, &rest);
        set_time_of_day(&made, wide_int64(rest));
        return new_iso(vm, (iso_value){ type, &made }, &call->result);
    }
    bool reject;
    if (overflow_rejects(vm, arg_of(call, 1u), &reject) != SJS_OK) return SJS_ERR_PROGRAM;
    int64_t days;
    if (type == SJS_TEMPORAL_PLAIN_DATE) {
        days = wide_int64(wide_quotient(d.time, (u128){ 0, (uint64_t)NS_PER_DAY }, &rest));
        rest = wide_of(0);
    } else {
        days = wide_int64(wide_floor(wide_add(d.time, wide_of(time_of_day(&made))), (u128){ 0, (uint64_t)NS_PER_DAY }, &rest));
    }
    int64_t added;
    if (add_date(vm, (date_addition){ &made, { d.date.years, d.date.months, d.date.weeks, days }, reject }, &added) != SJS_OK) return SJS_ERR_PROGRAM;
    date_of_days(added, &made);
    set_time_of_day(&made, wide_int64(rest));
    if (type == SJS_TEMPORAL_PLAIN_DATE_TIME && !within_limits(&made, true)) return outside_limits(vm);
    return new_iso(vm, (iso_value){ type, &made }, &call->result);
}

/* until(other, options) and since: the Duration from the receiver to
 * other (since: negated), rounded as difference_options say: a date's in
 * calendar units, a time's of the clock, a date and time's both. */
static sjs_result iso_difference(sjs_vm *vm, sjs_leaf_call *call, bool since) {
    const sjs_temporal *t = SJS_AS_TEMPORAL(call->receiver);
    sjs_temporal_type type = (sjs_temporal_type)t->h.flags;
    sjs_iso_date_time other;
    difference_settings s;
    if (convert(vm, (conversion){ type, { arg_of(call, 0), sjs_undefined() } }, &other) != SJS_OK) return SJS_ERR_PROGRAM;
    if (difference_options(vm, (difference_request){ arg_of(call, 1u), type, since }, &s) != SJS_OK) return SJS_ERR_PROGRAM;
    const sjs_iso_date_time *from = &t->as.iso;
    internal_duration d = { { 0, 0, 0, 0 }, wide_of(0) };
    time_unit largest = s.largest;
    if (type == SJS_TEMPORAL_PLAIN_TIME) {
        u128 increment = u128_mul((u128){ 0, (uint64_t)UNIT_NANOSECONDS[s.smallest] }, (uint64_t)s.increment);
        d.time = round_wide(wide_of(time_of_day(&other) - time_of_day(from)), (wide_rounding){ increment, s.mode });
    } else if (type == SJS_TEMPORAL_PLAIN_DATE_TIME) {
        if (difference_rounded(vm, (rounded_difference){ { from, &other }, s }, &d) != SJS_OK) return SJS_ERR_PROGRAM;
    } else if (compare_dates(from, &other) != 0) {
        largest = UNIT_DAY;
        d.date = date_until(from, &other, s.largest);
        if (s.smallest != UNIT_DAY || s.increment != 1) {
            relative_rounding r = { d, epoch_nanoseconds(days_of(&other), 0), from, s };
            if (round_relative(vm, &r, &d) != SJS_OK) return SJS_ERR_PROGRAM;
        }
    }
    double f[DURATION_FIELD_COUNT];
    fields_of_internal((duration_balance){ d, largest }, f);
    if (since) negate_fields(f);
    return new_duration(vm, f, &call->result);
}

/* ------------------------------------------------------------------------
 * Temporal.Instant: an exact time in nanoseconds since 1970-01-01T00:00Z,
 * within 8.64 x 10^21 of it (10^8 days) either way, held as whole seconds
 * (rounded toward negative infinity) and the nanoseconds past them. UTC
 * only: toString writes Z, or the offset of UTC or of a fixed offset.
 */

#define NS_PER_SECOND INT64_C(1000000000)

static wide epoch_of(const sjs_temporal *t) {
    return wide_add(wide_mul(wide_of(t->as.instant.seconds), NS_PER_SECOND), wide_of((int64_t)t->as.instant.nanoseconds));
}

/* IsValidEpochNanoseconds: at most 8.64 x 10^21 either way. */
static bool epoch_within_limits(wide ns) {
    return u128_cmp(ns.m, wide_mul(wide_of(INT64_C(8640000000000)), NS_PER_SECOND).m) <= 0;
}

static sjs_result outside_instants(sjs_vm *vm) {
    return sjs_vm_fault(vm, "JavaScript throws a RangeError for an Instant more than 8.64 x 10^21 nanoseconds from the epoch");
}

/* An Instant at `ns`, which must be within the limits. */
static sjs_result new_instant(sjs_vm *vm, wide ns, sjs_value *out) {
    if (!epoch_within_limits(ns)) return outside_instants(vm);
    sjs_temporal *t;
    sjs_result r = sjs_temporal_make(vm->context, SJS_TEMPORAL_INSTANT, &t);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    wide rest;
    t->as.instant.seconds = wide_int64(wide_floor(ns, (u128){ 0, (uint64_t)NS_PER_SECOND }, &rest));
    t->as.instant.nanoseconds = (uint32_t)rest.m.lo;
    *out = sjs_heap_value(SJS_TEMPORAL, &t->h);
    return SJS_OK;
}

/* An Instant's text: its date and time at `offset` nanoseconds (whole
 * minutes) from UTC, the seconds and fraction as `digits` says, then Z, or
 * with `zoned` the offset, +HH:MM (+00:00 for UTC). */
typedef struct {
    wide epoch;
    int64_t offset;
    int32_t digits;
    bool zoned;
} instant_text;

static uint32_t put_instant(char *out, instant_text t) {
    wide time;
    int64_t days = wide_int64(wide_floor(wide_add(t.epoch, wide_of(t.offset)), (u128){ 0, (uint64_t)NS_PER_DAY }, &time));
    sjs_iso_date_time d = date_at(days);
    set_time_of_day(&d, wide_int64(time));
    uint32_t n = put_iso(out, (iso_value){ SJS_TEMPORAL_PLAIN_DATE_TIME, &d }, t.digits);
    if (!t.zoned) {
        out[n++] = 'Z';
        return n;
    }
    int64_t minutes = t.offset / UNIT_NANOSECONDS[UNIT_MINUTE];
    out[n++] = minutes < 0 ? '-' : '+';
    if (minutes < 0) minutes = -minutes;
    n += put_digits(out + n, (uint64_t)(minutes / 60), 2u);
    out[n++] = ':';
    n += put_digits(out + n, (uint64_t)(minutes % 60), 2u);
    return n;
}

static uint32_t put_instant_text(const sjs_temporal *t, char *out) {
    return put_instant(out, (instant_text){ epoch_of(t), 0, DIGITS_AUTO, false });
}

/* An Instant's ISO 8601 string (TemporalInstantString), as both polyfills
 * read one: a date, T (or t or a space) and a time, as after a date; then Z
 * (or z) or a UTC offset, +HH to +HH:MM:SS.fffffffff, applied exactly; then
 * annotations as after a date and time, a time zone's and a calendar's
 * (any calendar) read and ignored. Its epoch nanoseconds into `out`, not
 * yet checked against the limits. */
static bool parse_instant(sjs_str text, wide *out) {
    iso_scan s = { text, 0, false, true };
    sjs_iso_date_time d = { 0 };
    if (!scan_date(&s, &d)) return false;
    int separator = scan_peek(&s);
    if (separator != 'T' && separator != 't' && separator != ' ') return false;
    s.at++;
    clock_parts time;
    if (!scan_clock(&s, (clock_grammar){ true, 60u }, &time)) return false;
    set_clock(&d, &time);
    int64_t offset = 0;
    if (!scan_take(&s, 'Z') && !scan_take(&s, 'z')) {
        int sign = scan_peek(&s);
        clock_parts o;
        if (sign != '+' && sign != '-') return false;
        s.at++;
        if (!scan_clock(&s, (clock_grammar){ true, 59u }, &o)) return false;
        offset = (((int64_t)o.hour * 60 + o.minute) * 60 + o.second) * NS_PER_SECOND + o.nanoseconds;
        if (sign == '-') offset = -offset;
    }
    if (!scan_annotations(&s)) return false;
    *out = wide_sub(epoch_nanoseconds(days_of(&d), time_of_day(&d)), wide_of(offset));
    return true;
}

/* ToTemporalInstant: an Instant's epoch, or a string's. JavaScript also
 * converts an object to a string (a ZonedDateTime gives its own instant),
 * which staticjs does not; any other value is a TypeError there. */
static sjs_result to_instant(sjs_vm *vm, sjs_value item, wide *out) {
    if (is_temporal(item, SJS_TEMPORAL_INSTANT)) {
        *out = epoch_of(SJS_AS_TEMPORAL(item));
        return SJS_OK;
    }
    if (item.kind == SJS_STRING) {
        sjs_str text = parsed_text(vm, item);
        if (!parse_instant(text, out)) {
            return unparsed(vm, "staticjs reads an Instant from an ISO 8601 date and time with Z or a UTC offset, and this is not one", text);
        }
        return epoch_within_limits(*out) ? SJS_OK : outside_instants(vm);
    }
    if (sjs_is_primitive(item)) return sjs_vm_fault(vm, "JavaScript throws a TypeError for an Instant from a value that is neither an Instant nor a string");
    return sjs_vm_fault(vm, "staticjs makes an Instant from an Instant or a string, where JavaScript would convert this object to a string");
}

/* ToBigInt of epoch nanoseconds: a BigInt, or a boolean (0n or 1n); within
 * the limits. JavaScript also parses a string and converts an object,
 * which staticjs does not; a number, undefined or null is a TypeError
 * there. */
static sjs_result epoch_argument(sjs_vm *vm, sjs_value v, wide *out) {
    if (v.kind == SJS_BOOLEAN) {
        *out = wide_of(v.boolean ? 1 : 0);
        return SJS_OK;
    }
    if (v.kind == SJS_BIGINT) {
        const sjs_bigint *b = SJS_AS_BIGINT(v);
        uint32_t limbs[3] = { 0, 0, 0 };
        if (b->length > 3u) return outside_instants(vm);
        for (uint32_t i = 0; i < b->length; i++) limbs[i] = b->limbs[i];
        *out = wide_make((u128){ limbs[2], ((uint64_t)limbs[1] << 32) | limbs[0] }, sjs_bigint_negative(b));
        return epoch_within_limits(*out) ? SJS_OK : outside_instants(vm);
    }
    if (v.kind == SJS_STRING || !sjs_is_primitive(v)) {
        return sjs_vm_fault(vm, "staticjs reads epoch nanoseconds from a BigInt, where JavaScript would convert this value to one");
    }
    return sjs_vm_fault(vm, "JavaScript throws a TypeError for epoch nanoseconds that are not a BigInt");
}

/* fromEpochMilliseconds' argument: ToNumber (a number, a boolean or null;
 * undefined is NaN), which must be an integer (a RangeError in JavaScript)
 * within the limits, 8.64 x 10^15. */
static sjs_result epoch_milliseconds_argument(sjs_vm *vm, sjs_value v, wide *out) {
    double d;
    if (sjs_vm_to_number(vm, v, &d) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!sjs_number_is_integer(d)) return sjs_vm_fault(vm, "JavaScript throws a RangeError for epoch milliseconds that are not an integer");
    if (d < -8.64e15 || d > 8.64e15) return outside_instants(vm);
    *out = wide_mul(wide_of((int64_t)d), 1000000);
    return SJS_OK;
}

/* new Temporal.Instant(epochNanoseconds), from, fromEpochMilliseconds,
 * fromEpochNanoseconds and compare. */
static sjs_result instant_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_value item = arg_of(call, 0);
    wide a;
    wide b;
    switch (id) {
    case SJS_BUILTIN_TEMPORAL_INSTANT:
    case SJS_BUILTIN_TEMPORAL_INSTANT_FROM_EPOCH_NANOSECONDS:
        if (epoch_argument(vm, item, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        return new_instant(vm, a, &call->result);
    case SJS_BUILTIN_TEMPORAL_INSTANT_FROM_EPOCH_MILLISECONDS:
        if (epoch_milliseconds_argument(vm, item, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        return new_instant(vm, a, &call->result);
    case SJS_BUILTIN_TEMPORAL_INSTANT_FROM:
        if (to_instant(vm, item, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        if (!is_temporal(item, SJS_TEMPORAL_INSTANT)) return new_instant(vm, a, &call->result);
        /* Equal, and immutable: the same value serves. */
        call->result = item;
        sjs_ref(item);
        return SJS_OK;
    default:
        if (to_instant(vm, item, &a) != SJS_OK || to_instant(vm, arg_of(call, 1u), &b) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_num((double)wide_cmp(a, b));
        return SJS_OK;
    }
}

/* A BigInt of a wide integer, through its hexadecimal digits. */
static sjs_result bigint_of_wide(sjs_vm *vm, wide v, sjs_value *out) {
    static const char HEX[] = "0123456789abcdef";
    char digits[32];
    for (uint32_t i = 0; i < 16u; i++) {
        digits[i] = HEX[(v.m.hi >> (60u - 4u * i)) & 15u];
        digits[16u + i] = HEX[(v.m.lo >> (60u - 4u * i)) & 15u];
    }
    sjs_result r = sjs_bigint_from_digits(vm->context, (sjs_bigint_digits){ { digits, 32u }, 16u, v.negative }, out);
    return r == SJS_OK ? SJS_OK : sjs_vm_fail(vm, r);
}

/* epochMilliseconds (rounded toward negative infinity, a number) and
 * epochNanoseconds (a BigInt). */
static sjs_result instant_field(sjs_vm *vm, instant_getter getter, sjs_value *out) {
    const sjs_temporal *t = getter.instant;
    if (getter.field == SJS_FIELD_EPOCH_NANOSECONDS) return bigint_of_wide(vm, epoch_of(t), out);
    *out = sjs_num((double)(t->as.instant.seconds * 1000 + (int64_t)(t->as.instant.nanoseconds / 1000000u)));
    return SJS_OK;
}

/* RoundTemporalInstant: the epoch rounded to a multiple of `r.increment`,
 * which divides a day, by `r.mode` as if it were positive
 * (RoundNumberToIncrementAsIfPositive: trunc toward negative infinity,
 * expand toward positive infinity). False where temporal-polyfill gives
 * another value: it rounds the time of day with the quotient's parity, and
 * below zero, at an odd quotient, trunc and expand, and halfExpand and
 * halfTrunc on a tie, go the other way (@js-temporal/polyfill follows the
 * specification). */
static bool round_epoch(wide x, rounding r, wide *out) {
    wide rest;
    wide q = wide_floor(x, (u128){ 0, (uint64_t)r.increment }, &rest);
    *out = x;
    if (wide_sign(rest) == 0) return true;
    int cmp = u128_cmp(u128_add(rest.m, rest.m), (u128){ 0, (uint64_t)r.increment });
    bool odd = (q.m.lo & 1u) != 0;
    bool by_sign = r.mode == MODE_TRUNC || r.mode == MODE_EXPAND || (cmp == 0 && (r.mode == MODE_HALF_EXPAND || r.mode == MODE_HALF_TRUNC));
    bool differs = wide_sign(q) < 0 && odd && by_sign;
    if (rounds_away((rounding_case){ r.mode, false, cmp, !odd })) q = wide_add(q, wide_of(1));
    *out = wide_mul(q, r.increment);
    return !differs;
}

static sjs_result rounded_differently(sjs_vm *vm) {
    return sjs_vm_fault(vm, "staticjs refuses this rounding of an Instant before 1970, which temporal-polyfill and the specification round differently");
}

/* round: to an hour at most, by an increment that divides a day's worth
 * of the unit (a day's 24 hours included). */
static sjs_result instant_round(sjs_vm *vm, wide epoch, sjs_leaf_call *call) {
    round_choice choice;
    if (round_options(vm, (unit_option){ arg_of(call, 0), { UNIT_NANOSECOND, UNIT_HOUR } }, &choice) != SJS_OK) return SJS_ERR_PROGRAM;
    int64_t per_day = NS_PER_DAY / UNIT_NANOSECONDS[choice.unit];
    if (choice.r.increment > per_day || per_day % choice.r.increment != 0) {
        return sjs_vm_fault(vm, "JavaScript throws a RangeError for a roundingIncrement that does not divide a day");
    }
    wide rounded;
    if (!round_epoch(epoch, (rounding){ choice.r.increment * UNIT_NANOSECONDS[choice.unit], choice.r.mode }, &rounded)) return rounded_differently(vm);
    return new_instant(vm, rounded, &call->result);
}

/* toString's timeZone: undefined (Z), or a string: UTC in any case of its
 * letters, or a UTC offset, +HH, +HHMM or +HH:MM (-00:00 is +00:00).
 * JavaScript also reads a time zone's name (with its data) and an ISO
 * string's time zone; staticjs has UTC only. A value that is not a string
 * is a TypeError there (staticjs has no ZonedDateTime). */
typedef struct {
    int64_t offset;   /* nanoseconds, whole minutes */
    bool zoned;
} time_zone;

static sjs_result time_zone_of(sjs_vm *vm, sjs_value v, time_zone *out) {
    *out = (time_zone){ 0, false };
    if (v.kind == SJS_UNDEFINED) return SJS_OK;
    if (v.kind != SJS_STRING) return sjs_vm_fault(vm, "JavaScript throws a TypeError for a timeZone that is neither a string nor a ZonedDateTime");
    sjs_str text = sjs_string_view(SJS_AS_STRING(v));
    out->zoned = true;
    if (equal_ignoring_case(text, SJS_STR("utc"))) return SJS_OK;
    iso_scan s = { text, 1u, false, false };
    clock_parts offset;
    if (text.length > 0 && (text.ptr[0] == '+' || text.ptr[0] == '-') && scan_clock(&s, (clock_grammar){ false, 59u }, &offset) && s.at == text.length) {
        out->offset = ((int64_t)offset.hour * 60 + offset.minute) * UNIT_NANOSECONDS[UNIT_MINUTE];
        if (text.ptr[0] == '-') out->offset = -out->offset;
        return SJS_OK;
    }
    return unparsed(vm, "staticjs has the time zone UTC and fixed UTC offsets only", text);
}

static sjs_result time_zone_option(sjs_vm *vm, sjs_value options, time_zone *out) {
    sjs_value v;
    if (option_of(vm, (option_name){ options, SJS_STR("timeZone") }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    return time_zone_of(vm, v, out);
}

/* ------------------------------------------------------------------------
 * Temporal.Now: the host clock (sjs_vm_set_clock, docs/temporal.md "Host
 * clock"), read and nothing else; a clock never set faults.
 */

/* instant(), timeZoneId() ("UTC": the host's time zone is UTC), and
 * plainDateISO(timeZone), plainDateTimeISO and plainTimeISO: the clock's
 * date and time at the time zone, read as toString's timeZone (undefined
 * is UTC). zonedDateTimeISO faults: there is no ZonedDateTime. */
static sjs_result now_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    static const sjs_temporal_type TYPES[] = { SJS_TEMPORAL_PLAIN_DATE, SJS_TEMPORAL_PLAIN_DATE_TIME, SJS_TEMPORAL_PLAIN_TIME };
    time_zone zone = { 0, false };
    if (id == SJS_BUILTIN_TEMPORAL_NOW_TIME_ZONE_ID) return temporal_string(vm, SJS_STR("UTC"), &call->result);
    if (id == SJS_BUILTIN_TEMPORAL_NOW_ZONED_DATE_TIME_ISO) {
        return sjs_vm_fault(vm, "staticjs has no Temporal.ZonedDateTime: it needs time zone data");
    }
    if (id != SJS_BUILTIN_TEMPORAL_NOW_INSTANT && time_zone_of(vm, arg_of(call, 0), &zone) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!vm->clock_set) return sjs_vm_fault(vm, "the host has not set the clock (sjs_vm_set_clock)");
    wide epoch = wide_add(wide_mul(wide_of(vm->clock.seconds), NS_PER_SECOND), wide_of((int64_t)vm->clock.nanoseconds));
    if (id == SJS_BUILTIN_TEMPORAL_NOW_INSTANT) return new_instant(vm, epoch, &call->result);
    wide time;
    int64_t days = wide_int64(wide_floor(wide_add(epoch, wide_of(zone.offset)), (u128){ 0, (uint64_t)NS_PER_DAY }, &time));
    sjs_iso_date_time d = date_at(days);
    set_time_of_day(&d, wide_int64(time));
    return new_iso(vm, (iso_value){ TYPES[id - SJS_BUILTIN_TEMPORAL_NOW_PLAIN_DATE_ISO], &d }, &call->result);
}

/* toString({ fractionalSecondDigits, roundingMode, smallestUnit,
 * timeZone }): rounded as round rounds (trunc by default) to the digits
 * written, then at the time zone's offset. */
static sjs_result instant_to_string(sjs_vm *vm, text_request request, sjs_value *out) {
    text_precision precision;
    time_zone zone;
    if (precision_of(vm, (precision_request){ request.options, UNIT_MINUTE }, &precision) != SJS_OK) return SJS_ERR_PROGRAM;
    if (time_zone_option(vm, request.options, &zone) != SJS_OK) return SJS_ERR_PROGRAM;
    wide rounded;
    if (!round_epoch(epoch_of(request.value), precision.r, &rounded)) return rounded_differently(vm);
    char text[SJS_TEMPORAL_TEXT_MAX];
    uint32_t n = put_instant(text, (instant_text){ rounded, zone.offset, precision.digits, zone.zoned });
    vm->fuel_used += n;
    return temporal_string(vm, (sjs_str){ text, n }, out);
}

/* add(duration) and subtract: of hours to nanoseconds only (years, months,
 * weeks or days fault: JavaScript throws a RangeError), within the limits.
 * There are no options. */
static sjs_result instant_add(sjs_vm *vm, sjs_leaf_call *call, bool subtract) {
    double f[DURATION_FIELD_COUNT];
    if (to_duration(vm, arg_of(call, 0), f) != SJS_OK) return SJS_ERR_PROGRAM;
    if (f[0] != 0 || f[1] != 0 || f[2] != 0 || f[3] != 0) {
        return sjs_vm_fault(vm, "JavaScript throws a RangeError for adding years, months, weeks or days to an Instant");
    }
    wide time = internal_of(f, false).time;
    return new_instant(vm, wide_add(epoch_of(SJS_AS_TEMPORAL(call->receiver)), subtract ? wide_neg(time) : time), &call->result);
}

/* until(other, options) and since: the nanoseconds from the receiver to
 * other (since: negated), rounded as difference_options say, balanced up
 * to largestUnit (a second by default, an hour at most). */
static sjs_result instant_difference(sjs_vm *vm, sjs_leaf_call *call, bool since) {
    wide other;
    difference_settings s;
    if (to_instant(vm, arg_of(call, 0), &other) != SJS_OK) return SJS_ERR_PROGRAM;
    if (difference_options(vm, (difference_request){ arg_of(call, 1u), SJS_TEMPORAL_INSTANT, since }, &s) != SJS_OK) return SJS_ERR_PROGRAM;
    u128 increment = u128_mul((u128){ 0, (uint64_t)UNIT_NANOSECONDS[s.smallest] }, (uint64_t)s.increment);
    wide difference = wide_sub(other, epoch_of(SJS_AS_TEMPORAL(call->receiver)));
    internal_duration d = { { 0, 0, 0, 0 }, round_wide(difference, (wide_rounding){ increment, s.mode }) };
    double f[DURATION_FIELD_COUNT];
    fields_of_internal((duration_balance){ d, s.largest }, f);
    if (since) negate_fields(f);
    return new_duration(vm, f, &call->result);
}

static sjs_result instant_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    const sjs_temporal *t = SJS_AS_TEMPORAL(call->receiver);
    wide other;
    switch (id) {
    case SJS_METHOD_TEMPORAL_EQUALS:
        if (to_instant(vm, arg_of(call, 0), &other) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_boolean(wide_cmp(epoch_of(t), other) == 0);
        return SJS_OK;
    case SJS_METHOD_TEMPORAL_ADD:
    case SJS_METHOD_TEMPORAL_SUBTRACT:
        return instant_add(vm, call, id == SJS_METHOD_TEMPORAL_SUBTRACT);
    case SJS_METHOD_TEMPORAL_UNTIL:
    case SJS_METHOD_TEMPORAL_SINCE:
        return instant_difference(vm, call, id == SJS_METHOD_TEMPORAL_SINCE);
    case SJS_METHOD_TEMPORAL_ROUND:
        return instant_round(vm, epoch_of(t), call);
    case SJS_METHOD_TEMPORAL_TO_STRING:
        return instant_to_string(vm, (text_request){ t, arg_of(call, 0) }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_JSON:
        return instant_to_string(vm, (text_request){ t, sjs_undefined() }, &call->result);
    case SJS_METHOD_TEMPORAL_TO_LOCALE_STRING:
        return sjs_vm_fault(vm, "toLocaleString depends on locale data, which staticjs does not have");
    case SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME_ISO:
        return sjs_vm_fault(vm, "staticjs has no Temporal.ZonedDateTime: it needs time zone data");
    default:
        return sjs_vm_fault(vm, "JavaScript throws a TypeError for valueOf of a Temporal value: compare with equals or compare");
    }
}

sjs_result sjs_temporal_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    if (call->receiver.kind != SJS_TEMPORAL) return sjs_vm_fault(vm, "this method is a Temporal value's");
    const sjs_temporal *t = SJS_AS_TEMPORAL(call->receiver);
    sjs_temporal_type type = (sjs_temporal_type)t->h.flags;
    if (type == SJS_TEMPORAL_DATE) return sjs_date_method(vm, id, call);
    if (type == SJS_TEMPORAL_DURATION) return duration_method(vm, id, call);
    if (type == SJS_TEMPORAL_INSTANT) return instant_method(vm, id, call);
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
        return iso_add(vm, call, id == SJS_METHOD_TEMPORAL_SUBTRACT);
    case SJS_METHOD_TEMPORAL_UNTIL:
    case SJS_METHOD_TEMPORAL_SINCE:
        return iso_difference(vm, call, id == SJS_METHOD_TEMPORAL_SINCE);
    case SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME:
        return sjs_vm_fault(vm, "staticjs has no Temporal.ZonedDateTime: it needs time zone data");
    case SJS_METHOD_TEMPORAL_WITH_CALENDAR:
        return sjs_vm_fault(vm, "staticjs has only the ISO 8601 calendar, and no withCalendar");
    default:
        return sjs_vm_fault(vm, "staticjs does not have Temporal.PlainYearMonth or Temporal.PlainMonthDay");
    }
}
