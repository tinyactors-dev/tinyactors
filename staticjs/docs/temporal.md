# Temporal values

How staticjs holds and computes Temporal's types: `Temporal.PlainDate`,
`PlainTime` and `PlainDateTime` (built), and `Duration`, `Instant` and
`Temporal.Now` (to build, on the same design). Read `builtins.md` and
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
`PLAIN_DATE`, `PLAIN_TIME`, `PLAIN_DATE_TIME`, `INSTANT`, `DURATION`).
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
  `sjs_temporal_text` gives the string, `sjs_temporal_type_name` the type.
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

`add` is also a set's method that changes its receiver, so `d.add(...)` on
a variable the function owns compiles to `MUTATE`, which hands the call
to `call()` with the variable's slot reference as its receiver (`mutate()`
in `vm.c`). `sjs_temporal_method` faults on any receiver that is not a
Temporal value, so the worker who builds `add` must make `mutate()` pass
the variable's value as the receiver of a method that does not change it
(a `MUTATION` hunk, with its case in `mutation.spec`).

A method ID is a byte (`sjs_name.methods`); `internal.h` asserts that the
last fits. `SJS_METHOD_FIRST` is 96, so functions have IDs below it.

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
and `sjs_temporal_function` finds the type by the first (`TYPES`); one
`construct_plain`, one `convert` (`to_date`, `to_time`, `to_date_time`:
what `from`, `compare`, `equals`, `withPlainTime` and `toPlainDateTime`
read) and one `compare_iso` serve the three.

To add a type's functions: IDs before `SJS_BUILTIN_FUNCTION_COUNT`, rows in
`FUNCTION_TABLE` (`{ "new", "Temporal.PlainTime", ... }` for the
constructor), `FUNCTION_VALUE`s, and cases in `function_call`.
`Temporal.Now.instant()` is `{ "Temporal.Now", "instant", ... }`.

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

## Testing

- `tests/spec/temporal_plain_date.spec`, `temporal_plain_time.spec` and
  `temporal_plain_date_time.spec`: cases written by hand, values from the
  polyfill. A case named "deliberate" faults where JavaScript gives a
  value.
- `tests/temporal_test.c`: sweeps from fixed lists (no randomness, at most
  10,000 cases per built-in), each a JavaScript expression. With
  `--print` it writes `Y` lines (the source, staticjs's outcome, and
  whether the fault is deliberate) that `mise run diff-node` compares with
  the polyfill: a fault where it throws, its value where staticjs gives
  one, and a fault on every deliberate line. Add each type's sweeps there:
  every option value claimed (overflow, rounding modes, units) against the
  polyfill.

## Status and notes for the next types

- PlainDate: built, but `add`, `subtract`, `until` and `since` (they take
  or give a Duration), which fault saying they are not built yet.
  `toZonedDateTime`, `toPlainYearMonth`, `toPlainMonthDay`, `withCalendar`
  and `toLocaleString` fault.
- PlainTime and PlainDateTime: built, but `add`, `subtract`, `until` and
  `since` (they fault as a PlainDate's do). A PlainDateTime's
  `toZonedDateTime` and `withCalendar` fault. Their arithmetic is on the
  time of day as an int64 of nanoseconds and on days since the epoch
  (`time_of_day`, `epoch_days`, `date_of_days`); a Duration's `add` to
  them can use the same, with `round_iso`'s day carry and limit check.
- Duration: its fields as doubles; `add` must make `mutate()` pass a
  receiver (above).
- Instant and Temporal.Now: the host clock (docs/builtins-status.md, the
  introduction); `Temporal.Now` is a namespace already.
