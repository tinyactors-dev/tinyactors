# Temporal values

How staticjs holds and computes Temporal's types: `Temporal.PlainDate`,
`PlainTime`, `PlainDateTime`, `Duration` and `Instant`, and
`Temporal.Now`, which reads the host clock (all built). Read `builtins.md` and
`extending.md` ("To add a value kind") first; this note says only what is
particular to Temporal.

Scope (the user's decision): the ISO 8601 calendar and UTC only. No
`ZonedDateTime`, no time zone but UTC, no calendar but `iso8601`. Where
JavaScript would take a time zone, a calendar annotation, another calendar
or a time zone name, staticjs gives exactly what the polyfill gives, or
faults; never another value.

## The oracle

Node 22 has no `Temporal`, and its `--harmony-temporal` draft is wrong in
places (a `total` of `P1Y2M` relative to 2024-01-31 gives 422 days, not
425). The oracle is temporal-polyfill 1.0.5, vendored unchanged in
`tests/vendor/temporal-polyfill` (README there: version, integrity,
sha256). `tests/diff-node.mjs` runs it in a `node:vm` context of its own
and makes its `Temporal` the global of every case, expression or module;
nothing is fetched when the tests run.

Every expected value is computed with that polyfill and, when the case is
written, with `@js-temporal/polyfill` 0.5.1 (the proposal's reference
polyfill) too. A case on which the two disagree is not used. Where they
disagree on what they accept, staticjs accepts neither reading: it faults.

## The representation

One value kind, `SJS_TEMPORAL` (appended to `sjs_kind`; `SJS_VERSION_MINOR`
7), heap type `SJS_HEAP_TEMPORAL`, struct `sjs_temporal` in
`src/internal.h`. The type is in `h.flags` (`sjs_temporal_type`:
`PLAIN_DATE`, `PLAIN_TIME`, `PLAIN_DATE_TIME`, `INSTANT`, `DURATION`; and
`DATE`, JavaScript's Date, which is not Temporal's: docs/date.md).
One block, fixed size, no references inside: release frees it, and no
limit bounds it. Its fields, by type:

| Type | Member of `as` | Fields |
|---|---|---|
| PlainDate | `iso` | `year` (int32, -271821 to 275760), `month` 1-12, `day`; the time fields 0 |
| PlainTime | `iso` | `hour` 0-23, `minute`, `second` 0-59, `millisecond`, `microsecond`, `nanosecond` 0-999; the date fields 0 |
| PlainDateTime | `iso` | both |
| Instant | `instant` | `seconds` (int64, floor of epoch nanoseconds / 10^9) and `nanoseconds` 0-999,999,999: the range is +-8.64e21 ns, past an int64 of nanoseconds |
| Duration | `duration[10]` | years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds: each an integer held exactly in a double, all of one sign (as the polyfill holds them) |

A Temporal value is immutable, as every staticjs value once shared: every
method makes a new value (`from` of a value of its own type gives that
value back, which no program can tell from a copy). Arithmetic on
nanoseconds past 2^53 must be exact: `src/bigint.c`, or a fixed-width
integer of 64-bit parts (no 128-bit division: `check-freestanding` refuses
it), and no libm.

As a value among the others:

- `typeof` is `"object"`; it is truthy; not a primitive. `===` on two
  faults (identity, as on objects: `equals` and `compare` are the way), as
  do `<`, `+` and a template or `String()` (which JavaScript turns into
  `toString()`, a conversion of an object). A set member or a map key
  faults (identity).
- It has no own properties: `{ ...d }` and `Object.assign({}, d)` are `{}`,
  `Object.hasOwn(d, "year")` is `false`, `Object.keys(d)` and `in` fault
  (as on any value but an object or an array).
- The writer writes it as its type's `from` of what `toString()` gives
  with no options: `Temporal.PlainDate.from("2024-01-31")`,
  `Temporal.PlainTime.from("12:30:00")` (the seconds always, the fraction
  as far as it is not zero), `Temporal.PlainDateTime.from("2024-01-31T12:30:00")`,
  `Temporal.Instant.from("2024-01-31T10:00:00Z")`,
  `Temporal.Duration.from("P1D")`. That is JavaScript that evaluates to an
  equal value; diff-node's `show()` writes the same.
  `sjs_temporal_text` gives the string (at most `SJS_TEMPORAL_TEXT_MAX`,
  128 bytes: a Duration's reaches about 110), `sjs_temporal_type_name`
  the type.
- `JSON.stringify` writes the ISO string, quoted (its `toJSON` is
  `toString()`), whatever the replacer's keys.

## Names and methods

`NAMES` (`src/builtins.c`) lists every name on the five prototypes. Each
entry has two Temporal columns:

- `methods[8]`, the Temporal value's method of the name: one
  `SJS_METHOD_TEMPORAL_*` per name, shared by every type that has a method
  of it (`toString` of a PlainDate and of a Duration is one ID). The IDs
  are a group in `sjs_builtin_id` after the numbers'; `sjs_builtin_call`
  sends `id <= SJS_METHOD_TEMPORAL_WITH_PLAIN_TIME` to
  `sjs_temporal_method`, which switches on the receiver's type.
- `field`, the getter of the name (`sjs_temporal_field`): `year`,
  `dayOfWeek`, `hours`, `epochNanoseconds`, ...

Which types have a method or a getter is `temporal.c`'s to say:
`METHOD_TYPES` and `FIELD_TYPES`, a bit per type. So on a value:

- a getter of its type reads (`sjs_temporal_property`, from
  `property_other` in `vm.c`); a name its type does not have is
  `undefined`, as in JavaScript (`plainDate.hour`);
- a method of its type is the callee of a call (`sjs_temporal_callee`, from
  `method()` in `vm.c`), and faults when read as a value, as does a name
  of `Object.prototype`; any other call faults.

`add` is also a set's method that changes its receiver, a name the two
kinds share (docs/mutation.md, "Shared names"). The compiler cannot tell
a Temporal value from a set by its name, so it refuses none of `add`'s
calls but a statement, and the run decides. On a variable the function
owns, `d.add(...)` compiles to `MUTATE`, and `mutate()` in `vm.c` calls a
method that changes nothing, a Temporal value's `add`, with the
variable's value as an ordinary call's receiver; on a capture, a global
or an import, it is an ordinary call. So `xs.map(n => start.add({ days: n
}))`, `const e = d; d.add(x)`, `[d.add(a), d.add(b)]` and a capture after
`d.add(x)` all give JavaScript's values, and `d.add(a).add(b)` goes on
from the first result, a value, as an ordinary call's receiver (its cases
are in `mutation.spec`). Two forms stay out: `d.add(x);` as a statement on
a capture, a global or an import is refused as a set's would be (it has no
effect anyway); and a chain through a path, `o.d.add(a).add(b)`, faults
when it runs, as a path call's chain needs the container back: write
`const e = o.d.add(a); e.add(b)`.

A method ID is a `uint16_t` (`sjs_name.methods`, `.mutation`); `internal.h`
asserts that the last fits. `SJS_METHOD_FIRST` is 128, so functions have IDs
below it, and an assert fails the build when `SJS_BUILTIN_FUNCTION_COUNT`
passes it.

## Functions, constructors and namespaces

`Temporal` is a namespace (`read_name`'s `spaces`, `src/compiler.c`), and
so is each of its types and `Now` (`NS_TEMPORAL_PLAIN_DATE`, ...,
`NS_TEMPORAL_NOW`; `space_names` holds `"Temporal.PlainDate"`, ...). So
`Temporal.PlainDate.from` is `sjs_builtin_lookup("Temporal.PlainDate",
"from")`: a row of `FUNCTION_TABLE` and an immortal value in `FUNCTIONS`,
an ID from `SJS_BUILTIN_TEMPORAL_FIRST` on, which `function_call` sends to
`sjs_temporal_function`. `Temporal` or `Temporal.PlainDate` without a
member after it is unsupported, as `Math` alone is.

`new Temporal.PlainDate(y, m, d)`: the parser reads `new Temporal.X` for
every type JavaScript has and sends `NEW_BEGIN` with the text
`"Temporal.X"`; the compiler looks the constructor up as
`sjs_builtin_lookup("new", "Temporal.X")` (no namespace is called `new`,
so no name a program writes reaches it), pushes it, and compiles the rest
as a call (`NEW_END` makes a `CALL`). A type with no constructor in the
table, and a program that binds `Temporal` itself, are unsupported.
Calling a constructor without `new` is unsupported (its namespace without
a member).

A plain type's constructor, `from` and `compare` are three IDs in a row,
and `sjs_temporal_function` finds the type by the first (`TYPES`), after
a Duration's three and an Instant's five (`instant_function`), which it
switches on; one
`construct_plain`, one `convert` (`to_date`, `to_time`, `to_date_time`:
what `from`, `compare`, `equals`, `withPlainTime` and `toPlainDateTime`
read) and one `compare_iso` serve the three.

To add a type's functions: IDs before `SJS_BUILTIN_FUNCTION_COUNT`, rows in
`FUNCTION_TABLE` (`{ "new", "Temporal.PlainTime", ... }` for the
constructor), `FUNCTION_VALUE`s, and cases in `function_call`.
`Temporal.Now.instant()` is `{ "Temporal.Now", "instant", ... }`
(`now_function`).

## Conversions, options, faults

What JavaScript throws for (a RangeError or a TypeError) faults. Beyond
that, staticjs faults where JavaScript would convert what it does not:

- A number field (the constructor's arguments, a property bag's `year`,
  `month`, `day`, `hour` to `nanosecond`; `roundingIncrement`) is
  ToIntegerWithTruncation of a number, a boolean or
  `null` (`sjs_vm_to_number`, then `trunc`); `undefined`, NaN and the
  infinities fault (a RangeError in JavaScript); a string, an array or an
  object faults (JavaScript converts it).
  A time argument of a constructor that is `undefined` is 0. A time field
  out of its range is brought into it with `overflow` `"constrain"` (the
  default, of a bag), and faults with `"reject"` and in a constructor.
- `monthCode` is read from a string only (`"M01"` to `"M12"`).
- A calendar is `undefined` or the string `"iso8601"` in any case of its
  ASCII letters; another calendar, and an ISO string JavaScript reads a
  calendar from, fault.
- Options are `undefined` or an object; a primitive faults (a TypeError).
  An object's own property is read; any other object (an array, a map)
  has none of Temporal's options, as in JavaScript. An option's value is
  `undefined` (its default) or a string, one of its choices, else a fault;
  a value JavaScript would convert to a string faults.
  `fractionalSecondDigits` is `"auto"` or a number, floored, 0 to 9; any
  other value faults (JavaScript converts it to a string, which is not
  `"auto"`). `smallestUnit` is a unit's name, singular or plural, of the
  units the method rounds to (`round`: a PlainTime's `hour` to
  `nanosecond`, a PlainDateTime's `day` to `nanosecond`; `toString`:
  `minute` to `nanosecond`); toString's `"auto"` faults on purpose
  (temporal-polyfill reads it as none, @js-temporal/polyfill refuses it).
  `roundingIncrement` must divide the next larger unit into more than one
  part (an hour's 24, a minute's and a second's 60, the smaller units'
  1,000; a day's increment is 1).
- A string where Temporal takes a date is parsed as Temporal parses it (the
  method's own conversion, as `Number(s)` reads a string). The grammar is
  what both polyfills accept and give the same value for (`temporal.c`,
  "ISO 8601 strings"): the specification's, plus the polyfill's limits (at
  most nine bracketed annotations; a string with a time must be within the
  date-time limits, for a PlainDate and a PlainDateTime; a PlainTime takes
  the time of any date). A time alone (`parse_time_only`) is the clock, an
  offset and annotations, after an optional `T` or `t`; without it, a
  time whose text up to its annotations is also a year and month (YYYY-MM,
  YYYYMM, month 01 to 12) or a month and day (MM-DD, MMDD, a day the month
  has in a leap year) is refused, as the grammar refuses it: `"2021-12"`,
  `"1214"`, `"0229"`, but not `"2021-13"` or `"0231"`. A time annotated
  with another calendar faults on purpose (both polyfills ignore it).

## Wide integers

A Duration's time reaches 2^53 seconds, 9 x 10^24 nanoseconds, and an
epoch 8.64 x 10^21: past an int64. `wide` (`temporal.c`, "Wide
integers") is a sign and a 128-bit magnitude of two 64-bit halves, every
operation built from 64-bit ones (a product from 32-bit halves, a
quotient bit by bit past 64 bits), as no 128-bit type is allowed (its
multiplication and division need a library). `round_wide` rounds one to
an increment of any size (a day by 10^9 is 8.64 x 10^22 ns) by the nine
modes, as `TimeDuration.round` does (`rounds_away` is
GetUnsignedRoundingMode and ApplyUnsignedRoundingMode). `ratio_double`
gives a quotient as the nearest double (`sjs_number_from_top`), which is
what @js-temporal/polyfill's 50-digit division gives: `total` and a
field past 2^53 (a Duration's nanoseconds as one double, as
`TemporalDurationFromInternal` makes them) are that.

## Durations

A Duration's ten fields are doubles, each an integer, all of one sign,
-0 made 0 (both polyfills do); `check_duration` is IsValidDuration, exact
as temporal-polyfill computes it in BigInts: years, months and weeks below
2^32, and the days (24 hours each) and time below 2^53 seconds (a field
past 10^25 ns faults before it is added). Arithmetic goes through the
internal form (`internal_duration`): the date units as int64s and the time
as one wide integer of nanoseconds; `fields_of_internal` balances it back
up to a largest unit (TemporalDurationFromInternal).

- The constructor and a bag's fields are ToIntegerIfIntegral: a number,
  a boolean or `null` that is an integer; a fraction, NaN and the
  infinities fault (a RangeError), and so do a string, an array and an
  object (JavaScript converts them). A bag (`from`, `with`, `add`,
  `compare`) is an object with at least one of the ten fields, or a
  Duration; anything else faults (a TypeError).
- A string (`parse_duration`) is what both polyfills read: a sign (`+` or
  `-`), `P`, whole years, months, weeks and days in that order, then `T`
  and hours, minutes and seconds, the last of them with a fraction of 1
  to 9 digits after `.` or `,`, spread over the smaller fields (`PT1.5H`
  is `PT1H30M`); the letters in either case; at least one part, and one
  after a `T`. Digits past 2^64 are 2^64 - 1, far past every limit.
- `add` and `subtract` take no `relativeTo` (the current specification):
  years, months or weeks in either fault (a RangeError); 24-hour days and
  time add, balanced up to the larger of the two largest units.
- `round` and `total` take `relativeTo`: a PlainDate, a PlainDateTime (its
  date), a string or a bag of a date (its time is read, and not used), at
  whose midnight the Duration starts. A time zone (a bag's `timeZone`, or
  `offset`, a string's bracketed annotation) makes JavaScript's
  relativeTo a ZonedDateTime, which staticjs does not have: it faults
  (deliberate, where JavaScript gives a value); a string with `Z` and no
  annotation is a RangeError. A string with a time must be within the
  date-time limits, as `PlainDate.from` reads one, which faults on
  `-271821-04-19T00:00`, whose date both polyfills read.
- Relative to a date, `round` is the difference from it to where the
  Duration reaches (`reached`: the years, months and weeks added,
  constrained, and the days and time as 24-hour days), rounded
  (`difference_rounded`, DifferencePlainDateTimeWithRounding), and `total`
  its total in the unit (`difference_total`). Without one, a calendar unit
  in the Duration or the options faults (a RangeError), and they round or
  total 24-hour days and time. `compare` counts years, months and weeks
  in days from `relativeTo` (DateDurationDays), and needs it for them.
- `toString` rounds the seconds to the digits it writes (`trunc` by
  default), carried up to the Duration's largest unit (a second at least);
  `smallestUnit` is a second or smaller.

## Differences, rounded

`date_until` is the ISO calendar's CalendarDateUntil (years, then months,
counted while the day, unconstrained, does not pass the end date; then the
days from the constrained date), `add_date` its CalendarDateAdd (the year
and month balanced, the day constrained or rejected, then weeks and days,
within the limits), and `difference_iso` DifferenceISODateTime (a day
borrowed when the time runs against the date). `round_relative` is the
specification's RoundRelativeDuration: `nudge_calendar`
(NudgeToCalendarUnit: the span from the smallest unit truncated to its
increment to the next multiple, where the destination falls, which
rounds, and whose fraction is the `total`), `nudge_time`
(NudgeToDayOrTime: days and time as nanoseconds) and `bubble`
(BubbleRelativeDuration: a rounding that reached the end of a larger unit
carries into it).

The polyfills give different values in two places. Counting months from
the end of a month across a shorter one (from January 31, 2024 to March 1
at 01:30, or a Duration of a month and 12 hours from it): the destination
then lies past the window from the truncated count to the next multiple,
and the current specification (ComputeNudgeWindow's `additionalShift`)
moves the window on by an increment, as temporal-polyfill and the
reference polyfill of tc39/proposal-temporal do; @js-temporal/polyfill
follows the older text, which rounds such a destination to the window's
start (a `trunc` of P1MT12H in months from 2024-01-31 gave PT0S). The
reference polyfill also carries the shifted result into a larger unit
where temporal-polyfill does not, so the specification is not settled
there. `nudge_calendar` faults whenever the destination is outside the
window, rather than give any of these values; where the shifted window
gives the same value in both polyfills that fault is needless, and
accepted. A `total` in months or years whose last bit temporal-polyfill's
floating-point division rounds differently: staticjs gives the
specification's exact ratio (and @js-temporal/polyfill's). No case tests
either (temporal_test leaves them out, `DIFFERING`).

## add, subtract, until and since

`iso_add` (AddDurationToDate, ...DateTime, ...Time): the Duration is read
as `from` reads one, negated for `subtract`. A PlainDate adds its years,
months and weeks and the time's whole days (toward zero) with `add_date`,
the day constrained or rejected (`overflow`); a PlainDateTime adds the
time first, and the days it carries (toward negative infinity) go with
the date, then within the date-time limits; a PlainTime adds the 24-hour
days and time around the clock, and leaves years, months and weeks out.

`iso_difference` (DifferenceTemporalPlainDate, ...DateTime, ...Time): the
other value is read as `from` reads one; `difference_options` is
GetDifferenceSettings: the units of the type (a date's day to year, a
time's nanosecond to hour), `smallestUnit` by default the least, a
`largestUnit` of "auto" (the default) the larger of a day (an hour for a
time) and `smallestUnit`, `roundingMode` `trunc`, negated for `since`
(`ceil` and `floor` swap, as do their half modes), whose result is
negated. A date's difference is `date_until`, rounded relative to it
unless to a day by 1; a date and time's is `difference_rounded`; a
time's is its nanoseconds rounded.

## Instant

An Instant is exact nanoseconds since 1970-01-01T00:00Z, at most 8.64 x
10^21 either way (`epoch_within_limits`, IsValidEpochNanoseconds), held as
whole seconds and the nanoseconds past them; `epoch_of` gives the wide
integer, `new_instant` checks the limits and makes one. UTC only.

- The constructor and `fromEpochNanoseconds` take ToBigInt of a BigInt
  (its at most three 32-bit limbs read into a wide integer) or a boolean;
  a string (which JavaScript parses) and an object (which it converts)
  fault on purpose; a number, `undefined` and `null` are a TypeError.
  `fromEpochMilliseconds` takes ToNumber of a number, a boolean or `null`,
  an integer (a fraction, NaN and `undefined` are a RangeError) within
  8.64 x 10^15. `epochNanoseconds` is a BigInt (`bigint_of_wide`, through
  its hexadecimal digits), `epochMilliseconds` the milliseconds rounded
  toward negative infinity.
- A string (`parse_instant`, TemporalInstantString) is a date, `T`, `t` or
  a space and a time as after a date, then `Z` (or `z`) or a UTC offset
  (`+HH` to `+HH:MM:SS.fffffffff`, seconds to 59), applied exactly; then
  annotations as after a date and time, of which a time zone and a
  calendar (any calendar: `iso_scan.any_calendar`) are read and ignored.
  Both polyfills read the same; where they differ (an offset's seconds of
  60 and an empty annotation or value, which temporal-polyfill reads; a
  time zone annotation after a calendar's; an offset's separators mixed,
  `+01:0030`, which @js-temporal/polyfill reads), staticjs faults. A
  ToTemporalInstant of anything but an Instant or a string faults (an
  object JavaScript would convert to a string, on purpose).
- `add` and `subtract` (AddDurationToInstant) take a Duration as `from`
  reads one, with no options; years, months, weeks or days in it are a
  RangeError, and the result must be within the limits. `until` and
  `since` (DifferenceTemporalInstant) round the nanoseconds between
  (`round_wide`, by the sign) to the settings of `difference_options`
  over the time units, largestUnit by default the larger of a second and
  smallestUnit.
- `round` (RoundTemporalInstant) takes a unit up to an hour and an
  increment that divides a day's worth of the unit, a day included (24
  hours, 1,440 minutes, ..., but at most 10^9: `increment_option`), and
  rounds as if the epoch were positive (`round_epoch`:
  RoundNumberToIncrementAsIfPositive, so `trunc` rounds toward negative
  infinity before 1970 and `expand` toward positive). `toString` rounds the
  same way to the digits it writes (`trunc` by default), then writes the
  date and time at the time zone's offset: `Z` without one, `+00:00` for
  `"UTC"` (in any case), or the offset of a fixed offset `+HH`, `+HHMM`,
  `+HH:MM` (`-00:00` is `+00:00`). A time zone's name (`Europe/Paris`,
  `Etc/UTC`, `GMT`) and an ISO string's time zone need time zone data:
  they fault on purpose, as do `toZonedDateTimeISO` and `toLocaleString`.

The polyfills round an Instant differently (both in `round` and in
`toString`): temporal-polyfill rounds the time of day with the parity of
the whole quotient, and where that quotient is odd and below zero its
`trunc` and `expand`, and `halfExpand` and `halfTrunc` on a tie, round the
other way from the specification (and @js-temporal/polyfill). `round_epoch`
says where; staticjs faults there, deliberately (`-1n` to 2 nanoseconds by
`trunc` is `0` in temporal-polyfill, `-2n` in the specification). An
increment past 10^9, which temporal-polyfill takes and the specification
refuses, faults too.

## Rounding

`round_to` rounds an int64 of nanoseconds to a multiple of an increment by
each of the nine modes, the modes relative to the sign (`expand` away from
zero, `trunc` toward it, `ceil` and `floor` toward the infinities, the
half modes on a tie only): it serves negative values too, and so a
Duration. `round_iso` rounds a time of day (below 86,400 * 10^9, an int64
with room) and carries a day into a PlainDateTime's date
(`date_of_days`, the inverse of `epoch_days`), which must stay within the
limits: rounding up past the greatest, or down onto -271821-04-19T00:00,
faults; a PlainTime wraps to midnight. `toString` rounds the same way to
the digits it writes (`roundingMode` `trunc` by default).

The polyfills differ on one kind of tie: `halfEven` makes the multiple
even counted from midnight in temporal-polyfill, counted from the start of
the unit above in @js-temporal/polyfill, as the specification's RoundTime
does (its quantity for a minute is the minute and what is below it, not the
hour; the reference polyfill of tc39/proposal-temporal agrees). They give
different values where the unit above holds an odd number of increments
(12, 20 or 4 minutes or seconds; 8, 40 or 200 of a smaller unit) on a tie
that is not in the unit's first span: `23:54` to 12 minutes is `23:48` in
the specification and `00:00` in temporal-polyfill. staticjs follows the
specification (`round_iso`). temporal_test leaves those ties out, since
diff-node's oracle is temporal-polyfill; they were checked against the
reference polyfill instead (the adversarial review's PlainTime sweep).

## Testing

- `tests/spec/temporal_plain_date.spec`, `temporal_plain_time.spec`,
  `temporal_plain_date_time.spec`, `temporal_duration.spec`,
  `temporal_instant.spec` and `temporal_now.spec` (`@clock`): cases
  written by hand, values from the polyfill. A case named "deliberate" faults where JavaScript gives a
  value.
- `tests/temporal_test.c`: sweeps from fixed lists (no randomness, at most
  10,000 cases per built-in), each a JavaScript expression. With
  `--print` it writes `Y` lines (the source, staticjs's outcome, and
  whether the fault is deliberate) that `mise run diff-node` compares with
  the polyfill: a fault where it throws, its value where staticjs gives
  one, and a fault on every deliberate line. Add each type's sweeps there:
  every option value claimed (overflow, rounding modes, units) against the
  polyfill. A Duration's: its strings (signs, date and time parts, each
  varied with the others, in both cases), `round` relative to five dates
  at month ends and leap days over every pair of units and every mode
  (and increments of the largest unit), `round` without one over every
  unit up to a day, its increments and modes and largest units, `total`
  in every unit, `toString`'s digits by every mode, `compare`, `add` and
  `subtract`. The other types': `until` and `since` between dates at month
  ends, leap days and the limits over every pair of units and mode (a
  date's by increments too), between dates and times on either side of
  each other's time, and between times over every increment; `add` and
  `subtract` of durations at month ends and leap days by each
  `overflow`. An Instant's: strings (dates, times, offsets and
  annotations, each varied with the others), epochs of every kind of
  value at and past the limits and their getters, `round` over every unit,
  increments that do and do not divide a day and every mode of instants
  at and beside the halves of each unit before and after 1970 and at the
  limits, `toString`'s digits by every mode and its time zones, `until` and
  `since` over every pair of units, increment and mode, `add`, `subtract`,
  `compare` and `equals`. Check them against @js-temporal/polyfill too, outside the
  tests: a value staticjs gives must be both polyfills', and a line on
  which they disagree must fault (deliberately, where temporal-polyfill
  gives a value) or be left out.

## Host clock

The current time is the host's: a read-only value that the evaluator
holds and the host pushes, as it pushes fuel (`include/staticjs.h`,
"The host clock"). `Temporal.Now` reads it and nothing else, and so do
`Date.now()` and `new Date()` (docs/date.md); staticjs reads no clock of its own and
calls back for none.

```c
typedef struct {
    int64_t seconds;       /* since 1970-01-01T00:00Z, toward negative infinity */
    uint32_t nanoseconds;  /* 0 to 999,999,999 */
} sjs_epoch;

sjs_result sjs_vm_set_clock(sjs_vm *vm, const sjs_epoch *now);
sjs_result sjs_vm_advance_clock(sjs_vm *vm, uint64_t nanoseconds);
sjs_result sjs_vm_clock(const sjs_vm *vm, sjs_epoch *out);
```

- **The evaluator holds it, not the context.** An evaluation is what has
  to be deterministic: the same program, globals and clock give the same
  result whichever other evaluations share the context, and two
  evaluators of one context can run at different times (one replaying a
  log, one live). A run calls nothing in the host (a native gets the
  context, not the evaluator), so two reads within one run agree, and
  two reads with no `sjs_vm_set_clock` or `sjs_vm_advance_clock` between
  them agree across any number of pauses. A paused evaluation resumed
  later sees the clock as the host set it before the resuming run: the
  host decides whether a pause lets time pass (advance it) or not (leave
  it).
- **It stays until set again**: across pauses, `sjs_vm_start` and
  `sjs_vm_reset`; `sjs_vm_destroy` drops it. A host that wants each
  evaluation at its own instant sets it before each `sjs_vm_start`.
  `sjs_vm_clock` reads it back, for a host that saves a paused
  evaluation's state.
- **Never set, a read faults**, with "the host has not set the clock
  (sjs_vm_set_clock)": a program reads the host's time or nothing, never
  a default. `sjs_vm_advance_clock` of a clock never set is
  `SJS_ERR_INVALID` (a call out of order).
- **Seconds and nanoseconds, not an int64 of nanoseconds.** An int64 of
  nanoseconds reaches 2^63 ns, 292 years either side of 1970 (1677 to
  2262); an Instant reaches 8.64 x 10^21 ns (-271821-04-20 to
  +275760-09-13), and `Date`'s 8.64 x 10^15 ms is the same span. A host
  that replays, simulates or tests far dates must be able to set any
  instant a program can make, so the clock is an Instant's own
  representation (`as.instant`: whole seconds, floored, and the
  nanoseconds past them), which `Temporal.Now.instant()` copies. Within
  the limits (the seconds within 8.64 x 10^12, and at the upper limit no
  nanoseconds), else `SJS_ERR_INVALID` and the clock unchanged.
- **Advanced by an unsigned 64-bit count of nanoseconds**: forward only
  (584 years at most per call); a host that moves its clock back, or
  further, sets it. Past the limits it is `SJS_ERR_INVALID`, the clock
  unchanged.
- No call allocates. The precision is the host's: staticjs gives every
  nanosecond it is given. (temporal-polyfill reads `Date.now()`, whole
  milliseconds; @js-temporal/polyfill adds to those a count of
  nanoseconds from an earlier `Date.now()`. The specification leaves the
  precision to the implementation, so neither is a value to match.)

`Temporal.Now` (`now_function`): `instant()` is an Instant of the clock;
`plainDateTimeISO(timeZone)`, `plainDateISO` and `plainTimeISO` its date
and time at the time zone, read as an Instant's `toString` reads its
`timeZone` (`time_zone_of`): `undefined` (the host's time zone, UTC),
`"UTC"` in any case, or a fixed offset `+HH`, `+HHMM`, `+HH:MM`; a time
zone's name and an ISO string (both polyfills read its time zone) fault
on purpose, and anything but a string is a TypeError there. The time
zone is read before the clock, as both polyfills do. `timeZoneId()` is
`"UTC"`, and reads no clock. `zonedDateTimeISO` faults (no
ZonedDateTime). Other arguments are ignored, as in JavaScript.

The spec format fixes the clock with `@clock <epoch nanoseconds>`
(`tests/spec_test.c`): spec_test sets it before each evaluation of the
case, and diff-node gives the polyfill's `Temporal.Now` the same instant:
the context's `Date.now()` the clock's milliseconds (floored), `TZ` UTC,
and each of the polyfill's own `Now` functions called as the case calls
it, with the nanoseconds past the millisecond added to its result (at
UTC or a fixed offset a nanosecond moves the date and time as it moves
the instant). A case without `@clock` that reads the clock faults in
staticjs (deliberately: Node reads the machine's clock), and the
diff-node `Date.now()` throws there.

`Math.random` reads a seed the same way ("Host seed", below).

## Host seed

`Math.random` draws from a seed the host pushes, held by the evaluator as
the clock is (`include/staticjs.h`, "The host seed"):

```c
typedef struct {
    uint64_t state[2];  /* V8's xorshift128+ state as of its last refill */
    uint32_t left;      /* of the 64 values that refill made, 0 to 64 */
} sjs_random;

sjs_result sjs_vm_set_seed(sjs_vm *vm, int64_t seed);
sjs_result sjs_vm_random(const sjs_vm *vm, sjs_random *out);
sjs_result sjs_vm_set_random(sjs_vm *vm, const sjs_random *state);
```

- **V8's generator, exactly** ("V8's is as good as any"): a seed gives
  the values `node --random-seed=<seed>` gives. V8 12.4
  (`src/numbers/math-random.cc`, `src/base/utils/random-number-generator.h`
  and `.cc`, followed in `number.c` with V8's notice): the state is
  `MurmurHash3(seed)` and `MurmurHash3(~seed)` (the 64-bit finalizer;
  the flag's int sign-extended), seeded the first time a context draws;
  a draw with no values left refills a cache of 64, each the next
  xorshift128+ step's first word as the bits of a double in [1, 2)
  (`state0 >> 12 | 0x3FF0000000000000`) minus 1, and draws take them from
  the last to the first. staticjs keeps no cache: its cursor starts at
  the state after the refill, which gives the cache's last value, and
  steps back one (invertible) xorshift128+ step per draw. No libm: the
  double is made from bits. Node's flag takes a nonzero int32 (0 there is
  "the machine's entropy", and a wider value wraps to 32 bits); every
  int64 is a seed here, and seed -1 gives an exact 0 as its 64th value, as
  V8 does (`MurmurHash3(0)` is 0).
- **Each draw advances the state; nothing else does.** A run calls
  nothing in the host, so pausing changes no value: an evaluation paused
  at every unit of fuel draws what one run whole draws.
- **It stays until set again**: across pauses, `sjs_vm_start` and
  `sjs_vm_reset`, as the clock does, so a second evaluation on the same
  evaluator continues the first one's sequence, and an evaluation reset
  after some draws keeps them drawn (V8's context draws on from where it
  was, too). A host that wants every evaluation to draw the same values
  sets the seed before each `sjs_vm_start`; `sjs_vm_destroy` drops it.
- **Saved and restored as a state, not a seed.** `sjs_vm_random` gives
  the state between draws, for a host that saves a paused evaluation and
  resumes it elsewhere; `sjs_vm_set_random` puts it back (a state of
  zeros, which no seed gives, or more than 64 left is `SJS_ERR_INVALID`
  and the generator unchanged). It is V8's own state and index, not the
  cache, which is 64 doubles and follows from them.
- **Never set, a draw faults**, with "the host has not set the seed
  (sjs_vm_set_seed)": a program draws from the host's seed or not at
  all, never from entropy of the library's. `sjs_vm_random` of a seed
  never set is `SJS_ERR_NOT_FOUND`. Reading `Math.random` as a value
  (`typeof Math.random`, `const r = Math.random`) needs no seed; calling
  it does. Its arguments are ignored, as in JavaScript.
- No call allocates, and a draw costs the call's unit of fuel.

The spec format fixes the seed with `@seed <n>` (`tests/spec_test.c`):
spec_test sets it before each evaluation of the case, and diff-node runs
the case in a child `node --random-seed=<n>` (V8 reads the seed only from
the flag), so `n` is a nonzero int32 and the case an expression; only
seeded cases spawn a process. `tests/spec/random.spec` has the cases:
seeds of both signs and the int32 limits, 200 draws across three refills,
the draws either side of a refill, draws in callbacks and with
arguments, `Math.random` read as a value, and the faults without a seed.

## Status and notes for the next types

- PlainDate: built. `toZonedDateTime`, `toPlainYearMonth`,
  `toPlainMonthDay`, `withCalendar` and `toLocaleString` fault.
- PlainTime and PlainDateTime: built. A PlainDateTime's `toZonedDateTime`
  and `withCalendar` fault. Their arithmetic is on the time of day as an
  int64 of nanoseconds and on days since the epoch (`time_of_day`,
  `epoch_days`, `date_of_days`), and a Duration's time as a wide integer.
- Duration: built, but `toLocaleString`, which faults (locale data), and
  a ZonedDateTime `relativeTo`.
- Instant: built, UTC only. `toZonedDateTimeISO`, `toLocaleString` and a
  `toString` time zone other than UTC or a fixed offset fault.
- Temporal.Now: built on the host clock ("Host clock"). `instant()` is
  `new_instant` of the clock's epoch nanoseconds; the ISO functions are
  its date and time at the time zone (`put_instant`'s arithmetic:
  `wide_floor` by a day, `date_at`, `set_time_of_day`), and
  `timeZoneId()` is `"UTC"`. `zonedDateTimeISO` faults. `Date.now()` and `new Date()`
  read the same clock (`vm->clock`), floored to milliseconds
  (docs/date.md).
